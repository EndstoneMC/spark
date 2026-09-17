#include "native/alloc/elf_import_hooks.h"

#include "native/alloc/linux_loader_handle.h"

#if !defined(__linux__) || !defined(__x86_64__)
#error "elf_import_hooks.cpp requires Linux x86-64"
#endif

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <ranges>
#include <sstream>
#include <string_view>
#include <type_traits>

#include <sys/mman.h>

namespace spark {
namespace {

constexpr std::size_t KMaxElfModules = 512;
constexpr std::size_t KMaxImportTargets = 65536;

struct Image {
    std::uintptr_t base = 0;
    std::uintptr_t load_begin = 0;
    std::uintptr_t load_end = 0;
    std::vector<ElfW(Phdr)> headers;
    std::string name;
    bool main_executable = false;
    bool pinned = false;
};

struct PinSet {
    std::vector<void *> handles;

    ~PinSet()
    {
        for (void *handle : std::ranges::reverse_view(handles)) {
            if (handle != nullptr) {
                ::dlclose(handle);
            }
        }
    }
};

std::string executablePath()
{
    char path[4096]{};
    const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    return length > 0 ? std::string(path, static_cast<std::size_t>(length)) : std::string("<main executable>");
}

bool populateImage(Image &image, const dl_phdr_info &info)
{
    image.base = static_cast<std::uintptr_t>(info.dlpi_addr);
    image.main_executable = info.dlpi_name == nullptr || info.dlpi_name[0] == '\0';
    image.name =
        info.dlpi_name != nullptr && info.dlpi_name[0] != '\0' ? std::string(info.dlpi_name) : executablePath();
    image.headers.assign(info.dlpi_phdr, info.dlpi_phdr + info.dlpi_phnum);
    image.load_begin = (std::numeric_limits<std::uintptr_t>::max)();
    image.load_end = 0;
    for (const ElfW(Phdr) &header : image.headers) {
        if (header.p_type != PT_LOAD) {
            continue;
        }
        image.load_begin = (std::min)(image.load_begin, image.base + static_cast<std::uintptr_t>(header.p_vaddr));
        image.load_end =
            (std::max)(image.load_end, image.base + static_cast<std::uintptr_t>(header.p_vaddr + header.p_memsz));
    }
    return image.load_begin != (std::numeric_limits<std::uintptr_t>::max)() && image.load_end > image.load_begin;
}

int collectImages(dl_phdr_info *info, std::size_t, void *opaque)
{
    auto &images = *static_cast<std::vector<Image> *>(opaque);
    if (images.size() == KMaxElfModules) {
        return 1;
    }
    Image image;
    if (populateImage(image, *info)) {
        images.push_back(std::move(image));
    }
    return 0;
}

struct RefreshContext {
    Image *image = nullptr;
    bool found = false;
};

int refreshImage(dl_phdr_info *info, std::size_t, void *opaque)
{
    auto &context = *static_cast<RefreshContext *>(opaque);
    const auto base = static_cast<std::uintptr_t>(info->dlpi_addr);
    const bool main_executable = info->dlpi_name == nullptr || info->dlpi_name[0] == '\0';
    if (context.image == nullptr || base != context.image->base || main_executable != context.image->main_executable) {
        return 0;
    }
    if (!main_executable && std::string_view(info->dlpi_name) != context.image->name) {
        return 0;
    }
    Image refreshed;
    if (!populateImage(refreshed, *info)) {
        return 0;
    }
    refreshed.pinned = true;
    *context.image = std::move(refreshed);
    context.found = true;
    return 1;
}

bool pinImage(Image &image, PinSet &pins)
{
    gateway::LoaderHandle handle(image.main_executable ? ::dlopen(nullptr, RTLD_NOW)
                                                       : ::dlopen(image.name.c_str(), RTLD_NOW | RTLD_NOLOAD));
    gateway::loaderFault(50);
    if (!handle) {
        return false;
    }
    link_map *map = nullptr;
    if (::dlinfo(handle.get(), RTLD_DI_LINKMAP, static_cast<void *>(&map)) != 0 || map == nullptr ||
        static_cast<std::uintptr_t>(map->l_addr) != image.base) {
        return false;
    }
    RefreshContext context{.image = &image, .found = false};
    ::dl_iterate_phdr(refreshImage, &context);
    if (!context.found) {
        return false;
    }
    gateway::loaderFault(51);
    pins.handles.push_back(handle.get());
    handle.release();
    return true;
}

bool contains(const Image &image, std::uintptr_t address, std::size_t bytes = 1) noexcept
{
    return address >= image.load_begin && address < image.load_end && bytes <= image.load_end - address;
}

std::uintptr_t dynamicPointer(const Image &image, ElfW(Addr) value) noexcept
{
    const auto address = static_cast<std::uintptr_t>(value);
    return contains(image, address) ? address : image.base + address;
}

bool supportedRelocation(unsigned type) noexcept
{
    return type == R_X86_64_JUMP_SLOT || type == R_X86_64_GLOB_DAT || type == R_X86_64_64;
}

bool isSparkImage(const Image &image, std::uintptr_t replacement_base)
{
    Dl_info self{};
    ::dladdr(reinterpret_cast<void *>(&isSparkImage), &self);
    return !image.main_executable &&
           (image.base == replacement_base || image.base == reinterpret_cast<std::uintptr_t>(self.dli_fbase));
}

bool isLoaderImage(std::string_view path)
{
    const std::size_t separator = path.find_last_of('/');
    const std::string_view basename = path.substr(separator == std::string_view::npos ? 0 : separator + 1);
    return basename.find("linux-vdso") != std::string_view::npos ||
           basename.find("ld-linux") != std::string_view::npos || basename.starts_with("ld-");
}

struct MapRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    int protection = 0;
};

std::vector<MapRange> readMemoryMap()
{
    std::vector<MapRange> ranges;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream parser(line);
        std::string range;
        std::string permissions;
        if (!(parser >> range >> permissions)) {
            continue;
        }
        const std::size_t separator = range.find('-');
        if (separator == std::string::npos) {
            continue;
        }
        MapRange mapped;
        mapped.begin = std::stoull(range.substr(0, separator), nullptr, 16);
        mapped.end = std::stoull(range.substr(separator + 1), nullptr, 16);
        mapped.protection |= !permissions.empty() && permissions[0] == 'r' ? PROT_READ : 0;
        mapped.protection |= permissions.size() > 1 && permissions[1] == 'w' ? PROT_WRITE : 0;
        mapped.protection |= permissions.size() > 2 && permissions[2] == 'x' ? PROT_EXEC : 0;
        ranges.push_back(mapped);
    }
    return ranges;
}

int protectionForAddress(const std::vector<MapRange> &ranges, std::uintptr_t address) noexcept
{
    auto found = std::ranges::find_if(
        ranges, [address](const MapRange &range) { return address >= range.begin && address < range.end; });
    return found == ranges.end() ? -1 : found->protection;
}

std::string systemError(const char *operation)
{
    return std::string(operation) + " failed: " + std::strerror(errno);
}

bool sameModule(const Image &image, std::uintptr_t base, const std::string &name)
{
    return image.base == base && image.name == name;
}

}  // namespace

bool ElfImportHooks::prepare(std::span<const ElfImportHookSpec> specs, std::string &error)
{
    error.clear();
    const bool same_specs =
        prepared_ && specs.size() == specs_.size() &&
        std::equal(specs.begin(), specs.end(), specs_.begin(), [](const auto &left, const auto &right) {
            return left.replacement == right.replacement && left.name != nullptr && right.name != nullptr &&
                   std::strcmp(left.name, right.name) == 0 &&
                   (left.original == nullptr || left.original == right.original);
        });
    if (same_specs) {
        return rescan(error);
    }
    if (installed_) {
        error = "cannot replace installed ELF import specifications";
        return false;
    }
    if (specs.empty()) {
        error = "no ELF import hooks were requested";
        return false;
    }

    specs_.assign(specs.begin(), specs.end());
    for (auto &spec : specs_) {
        if (spec.original == nullptr && spec.name != nullptr) {
            spec.original = ::dlsym(RTLD_DEFAULT, spec.name);
        }
        if (spec.original == nullptr || spec.replacement == nullptr) {
            error = "ELF import hook requires a resolved original and replacement";
            return false;
        }
    }
    targets_.clear();
    pages_.clear();
    capabilities_.clear();
    if (!scan(error)) {
        specs_.clear();
        targets_.clear();
        pages_.clear();
        capabilities_.clear();
        return false;
    }
    prepared_ = true;
    return true;
}

bool ElfImportHooks::rescan(std::string &error)
{
    error.clear();
    if (!prepared_) {
        error = "ELF import hooks have not been prepared";
        return false;
    }
    if (!scan(error)) {
        return false;
    }
    if (installed_ && !patch(true, error)) {
        return false;
    }
    return true;
}

bool ElfImportHooks::scan(std::string &error)
{
    error.clear();
    std::vector<Image> images;
    images.reserve(64);
    ::dl_iterate_phdr(collectImages, &images);
    if (images.empty()) {
        error = "could not enumerate loaded Linux ELF images";
        return false;
    }

    const auto page_size_value = ::sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) {
        error = "sysconf(_SC_PAGESIZE) failed";
        return false;
    }
    const auto page_size = static_cast<std::uintptr_t>(page_size_value);

    std::uintptr_t replacement_base = 0;
    if (!specs_.empty() && specs_.front().replacement != nullptr) {
        Dl_info replacement{};
        if (::dladdr(specs_.front().replacement, &replacement) != 0) {
            replacement_base = reinterpret_cast<std::uintptr_t>(replacement.dli_fbase);
        }
    }

    std::vector<std::uintptr_t> allocator_bases;
    for (const ElfImportHookSpec &spec : specs_) {
        if (spec.name == nullptr) {
            continue;
        }
        void *address = spec.original;
        Dl_info owner{};
        if (address != nullptr && ::dladdr(address, &owner) != 0) {
            allocator_bases.push_back(reinterpret_cast<std::uintptr_t>(owner.dli_fbase));
        }
    }
    std::ranges::sort(allocator_bases);
    const auto duplicate = std::ranges::unique(allocator_bases);
    allocator_bases.erase(duplicate.begin(), duplicate.end());

    const std::vector<Target> previous_targets = targets_;
    std::vector<Target> next_targets;
    next_targets.reserve(previous_targets.size() + 32);
    std::vector<Page> next_pages;
    std::vector<std::pair<std::uintptr_t, std::string>> failed_modules;
    std::size_t skipped_modules = 0;
    auto mark_failed = [&](std::uintptr_t base, const std::string &name) {
        const auto key = std::pair<std::uintptr_t, std::string>{base, name};
        if (std::ranges::find(failed_modules, key) == failed_modules.end()) {
            failed_modules.push_back(key);
        }
    };

    PinSet pins;
    for (Image &image : images) {
        const bool already_hooked = std::ranges::any_of(previous_targets, [&image](const Target &target) {
            return target.module_base == image.base && target.module_name == image.name &&
                   target.main_executable == image.main_executable;
        });
        if (isSparkImage(image, replacement_base) || std::ranges::binary_search(allocator_bases, image.base) ||
            (!image.main_executable && isLoaderImage(image.name))) {
            ++skipped_modules;
            continue;
        }
        if (!pinImage(image, pins)) {
            mark_failed(image.base, image.name);
            continue;
        }
        if (scan_module_gate_ != nullptr && !scan_module_gate_(image.name)) {
            mark_failed(image.base, image.name);
            continue;
        }

        const ElfW(Dyn) *dynamic = nullptr;
        for (const ElfW(Phdr) &header : image.headers) {
            if (header.p_type == PT_DYNAMIC) {
                const std::uintptr_t address = image.base + static_cast<std::uintptr_t>(header.p_vaddr);
                if (contains(image, address, sizeof(ElfW(Dyn)))) {
                    // NOLINTNEXTLINE(performance-no-int-to-ptr)
                    dynamic = reinterpret_cast<const ElfW(Dyn) *>(address);
                }
                break;
            }
        }
        if (dynamic == nullptr) {
            ++skipped_modules;
            continue;
        }

        const ElfW(Sym) *symbols = nullptr;
        std::size_t symbol_entry_size = sizeof(ElfW(Sym));
        const char *strings = nullptr;
        std::size_t string_size = 0;
        const ElfW(Rel) *rel = nullptr;
        std::size_t rel_size = 0;
        std::size_t rel_entry_size = sizeof(ElfW(Rel));
        const ElfW(Rela) *rela = nullptr;
        std::size_t rela_size = 0;
        std::size_t rela_entry_size = sizeof(ElfW(Rela));
        const void *jmprel = nullptr;
        std::size_t jmprel_size = 0;
        ElfW(Sword) plt_type = DT_RELA;
        bool dynamic_valid = false;
        const std::size_t max_dynamic =
            (image.load_end - reinterpret_cast<std::uintptr_t>(dynamic)) / sizeof(ElfW(Dyn));
        for (std::size_t i = 0; i < max_dynamic; ++i) {
            const ElfW(Dyn) &entry = dynamic[i];
            if (entry.d_tag == DT_NULL) {
                dynamic_valid = true;
                break;
            }
            switch (entry.d_tag) {
            case DT_SYMTAB:
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                symbols = reinterpret_cast<const ElfW(Sym) *>(dynamicPointer(image, entry.d_un.d_ptr));
                break;
            case DT_STRTAB:
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                strings = reinterpret_cast<const char *>(dynamicPointer(image, entry.d_un.d_ptr));
                break;
            case DT_STRSZ:
                string_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_SYMENT:
                symbol_entry_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_REL:
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                rel = reinterpret_cast<const ElfW(Rel) *>(dynamicPointer(image, entry.d_un.d_ptr));
                break;
            case DT_RELSZ:
                rel_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_RELENT:
                rel_entry_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_RELA:
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                rela = reinterpret_cast<const ElfW(Rela) *>(dynamicPointer(image, entry.d_un.d_ptr));
                break;
            case DT_RELASZ:
                rela_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_RELAENT:
                rela_entry_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_JMPREL:
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                jmprel = reinterpret_cast<const void *>(dynamicPointer(image, entry.d_un.d_ptr));
                break;
            case DT_PLTRELSZ:
                jmprel_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_PLTREL:
                plt_type = static_cast<ElfW(Sword)>(entry.d_un.d_val);
                break;
            default:
                break;
            }
        }
        if (!dynamic_valid || symbols == nullptr || strings == nullptr || string_size == 0 ||
            symbol_entry_size != sizeof(ElfW(Sym)) ||
            !contains(image, reinterpret_cast<std::uintptr_t>(symbols), sizeof(ElfW(Sym))) ||
            !contains(image, reinterpret_cast<std::uintptr_t>(strings), string_size) ||
            (rel != nullptr && rel_entry_size != sizeof(ElfW(Rel))) ||
            (rela != nullptr && rela_entry_size != sizeof(ElfW(Rela)))) {
            mark_failed(image.base, image.name);
            continue;
        }

        const std::size_t before = next_targets.size();
        auto visit = [&](auto *entries, std::size_t bytes) {
            using Relocation = std::remove_cv_t<std::remove_pointer_t<decltype(entries)>>;
            const auto entries_address = reinterpret_cast<std::uintptr_t>(entries);
            if (entries == nullptr || bytes % sizeof(Relocation) != 0 || !contains(image, entries_address, bytes)) {
                return;
            }
            const std::size_t count = bytes / sizeof(Relocation);
            for (std::size_t i = 0; i < count; ++i) {
                const Relocation &relocation = entries[i];
                const auto type = static_cast<unsigned>(ELF64_R_TYPE(relocation.r_info));
                if (!supportedRelocation(type)) {
                    continue;
                }
                const auto symbol_index = static_cast<std::size_t>(ELF64_R_SYM(relocation.r_info));
                const auto symbols_address = reinterpret_cast<std::uintptr_t>(symbols);
                if (symbol_index >
                    ((std::numeric_limits<std::uintptr_t>::max)() - symbols_address) / sizeof(ElfW(Sym))) {
                    continue;
                }
                const std::uintptr_t symbol_address = symbols_address + symbol_index * sizeof(ElfW(Sym));
                if (!contains(image, symbol_address, sizeof(ElfW(Sym)))) {
                    continue;
                }
                const ElfW(Sym) &symbol = symbols[symbol_index];
                if (symbol.st_name >= string_size) {
                    continue;
                }
                const char *name = strings + symbol.st_name;
                const std::size_t remaining = string_size - symbol.st_name;
                if (std::memchr(name, '\0', remaining) == nullptr) {
                    continue;
                }
                for (std::size_t spec_index = 0; spec_index < specs_.size(); ++spec_index) {
                    const ElfImportHookSpec &spec = specs_[spec_index];
                    if (spec.name == nullptr || std::strcmp(name, spec.name) != 0) {
                        continue;
                    }
                    const std::uintptr_t slot_address = image.base + static_cast<std::uintptr_t>(relocation.r_offset);
                    if (!contains(image, slot_address, sizeof(void *)) || slot_address % alignof(void *) != 0) {
                        continue;
                    }
                    // NOLINTNEXTLINE(performance-no-int-to-ptr)
                    auto **slot = reinterpret_cast<void **>(slot_address);
                    auto existing = std::ranges::find_if(next_targets, [slot, &image](const Target &target) {
                        return target.slot == slot && target.module_base == image.base &&
                               target.module_name == image.name && target.main_executable == image.main_executable;
                    });
                    if (existing != next_targets.end()) {
                        continue;
                    }
                    if (next_targets.size() == KMaxImportTargets) {
                        continue;
                    }
                    void *current = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
                    void *original = current;
                    if (current == spec.replacement) {
                        auto previous = std::ranges::find_if(previous_targets, [&](const Target &target) {
                            return target.slot == slot && target.module_base == image.base &&
                                   target.module_name == image.name &&
                                   target.main_executable == image.main_executable && target.spec_index == spec_index &&
                                   target.replacement == spec.replacement;
                        });
                        if (previous != previous_targets.end()) {
                            original = previous->original;
                        }
                    }
                    if (original != spec.original) {
                        continue;
                    }
                    next_targets.push_back(
                        {slot, original, spec.replacement, spec_index, image.base, image.name, image.main_executable});
                }
            }
        };
        visit(rel, rel_size);
        visit(rela, rela_size);
        if (plt_type == DT_REL) {
            visit(static_cast<const ElfW(Rel) *>(jmprel), jmprel_size);
        }
        else if (plt_type == DT_RELA) {
            visit(static_cast<const ElfW(Rela) *>(jmprel), jmprel_size);
        }
        else if (jmprel != nullptr) {
            mark_failed(image.base, image.name);
        }

        if (next_targets.size() == before && !already_hooked) {
            ++skipped_modules;
        }
    }

    for (const Target &previous : previous_targets) {
        const auto failed = std::ranges::find(
            failed_modules, std::pair<std::uintptr_t, std::string>{previous.module_base, previous.module_name});
        if (failed == failed_modules.end()) {
            continue;
        }
        auto image = std::ranges::find_if(images, [&previous](const Image &candidate) {
            return candidate.pinned && candidate.base == previous.module_base &&
                   candidate.name == previous.module_name && candidate.main_executable == previous.main_executable;
        });
        if (image == images.end() ||
            !contains(*image, reinterpret_cast<std::uintptr_t>(previous.slot), sizeof(void *)) ||
            __atomic_load_n(previous.slot, __ATOMIC_ACQUIRE) != previous.replacement) {
            continue;
        }
        const bool duplicate_target = std::ranges::any_of(next_targets, [&previous](const Target &target) {
            return target.slot == previous.slot && target.module_base == previous.module_base &&
                   target.module_name == previous.module_name && target.spec_index == previous.spec_index;
        });
        if (!duplicate_target && next_targets.size() < KMaxImportTargets) {
            next_targets.push_back(previous);
        }
    }

    const std::vector<MapRange> memory_map = readMemoryMap();
    std::vector<std::pair<std::uintptr_t, std::string>> page_failed_modules;
    for (const Target &target : next_targets) {
        const std::uintptr_t page_address = reinterpret_cast<std::uintptr_t>(target.slot) & ~(page_size - 1);
        const int protection = protectionForAddress(memory_map, reinterpret_cast<std::uintptr_t>(target.slot));
        if (protection < 0) {
            mark_failed(target.module_base, target.module_name);
            const auto key = std::pair<std::uintptr_t, std::string>{target.module_base, target.module_name};
            if (std::ranges::find(page_failed_modules, key) == page_failed_modules.end()) {
                page_failed_modules.push_back(key);
            }
            continue;
        }
        auto existing_page = std::ranges::find_if(next_pages, [page_address, &target](const Page &page) {
            return reinterpret_cast<std::uintptr_t>(page.address) == page_address &&
                   page.module_base == target.module_base && page.module_name == target.module_name &&
                   page.main_executable == target.main_executable;
        });
        if (existing_page == next_pages.end()) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            next_pages.push_back({reinterpret_cast<void *>(page_address), protection, target.module_base,
                                  target.module_name, target.main_executable});
        }
        else {
            existing_page->protection = protection;
        }
    }
    if (!page_failed_modules.empty()) {
        const auto removed_targets = std::ranges::remove_if(next_targets, [&page_failed_modules](const Target &target) {
            return std::ranges::find(page_failed_modules,
                                     std::pair<std::uintptr_t, std::string>{target.module_base, target.module_name}) !=
                   page_failed_modules.end();
        });
        next_targets.erase(removed_targets.begin(), removed_targets.end());
        const auto removed_pages = std::ranges::remove_if(next_pages, [&page_failed_modules](const Page &page) {
            return std::ranges::find(page_failed_modules,
                                     std::pair<std::uintptr_t, std::string>{page.module_base, page.module_name}) !=
                   page_failed_modules.end();
        });
        next_pages.erase(removed_pages.begin(), removed_pages.end());
    }

    std::vector<ElfImportHookCapability> next_capabilities;
    next_capabilities.reserve(specs_.size());
    std::string required_error;
    for (std::size_t i = 0; i < specs_.size(); ++i) {
        ElfImportHookCapability capability;
        capability.name = specs_[i].name != nullptr ? specs_[i].name : "";
        capability.slots = static_cast<std::size_t>(std::count_if(
            next_targets.begin(), next_targets.end(), [i](const Target &target) { return target.spec_index == i; }));
        capability.available = capability.slots != 0;
        if (!capability.available) {
            capability.detail = "no resolved import matches the selected allocator provider";
            if (specs_[i].required && required_error.empty()) {
                required_error = "required Linux allocator import not found: " + capability.name;
            }
        }
        next_capabilities.push_back(std::move(capability));
    }

    targets_.swap(next_targets);
    pages_.swap(next_pages);
    capabilities_.swap(next_capabilities);
    skipped_modules_ = skipped_modules;
    failed_modules_ = failed_modules.size();

    std::vector<std::pair<std::uintptr_t, std::string_view>> hooked;
    for (const Target &target : targets_) {
        const bool loaded = std::ranges::any_of(images, [&target](const Image &image) {
            return image.pinned && sameModule(image, target.module_base, target.module_name) &&
                   image.main_executable == target.main_executable;
        });
        if (!loaded) {
            continue;
        }
        const auto key = std::pair<std::uintptr_t, std::string_view>{target.module_base, target.module_name};
        if (std::ranges::find(hooked, key) == hooked.end()) {
            hooked.push_back(key);
        }
    }
    hooked_modules_ = hooked.size();
    if (!required_error.empty()) {
        error = std::move(required_error);
        return false;
    }
    return true;
}

bool ElfImportHooks::patch(bool replacements, std::string &error)
{
    error.clear();
    const auto page_size_value = ::sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) {
        error = "sysconf(_SC_PAGESIZE) failed";
        return false;
    }
    const auto page_size = static_cast<std::uintptr_t>(page_size_value);

    std::vector<Image> images;
    images.reserve(64);
    ::dl_iterate_phdr(collectImages, &images);
    PinSet pins;
    for (Image &image : images) {
        const bool referenced = std::ranges::any_of(targets_, [&image](const Target &target) {
            return target.module_base == image.base && target.module_name == image.name &&
                   target.main_executable == image.main_executable;
        });
        if (referenced) {
            pinImage(image, pins);
        }
    }

    std::vector<const Target *> updates;
    updates.reserve(targets_.size());
    std::vector<std::pair<std::uintptr_t, std::string_view>> hooked;
    for (Target &target : targets_) {
        auto image = std::ranges::find_if(images, [&target](const Image &candidate) {
            return candidate.pinned && candidate.base == target.module_base && candidate.name == target.module_name &&
                   candidate.main_executable == target.main_executable;
        });
        if (image == images.end() || !contains(*image, reinterpret_cast<std::uintptr_t>(target.slot), sizeof(void *))) {
            continue;
        }
        void *current = __atomic_load_n(target.slot, __ATOMIC_ACQUIRE);
        if (replacements) {
            const auto key = std::pair<std::uintptr_t, std::string_view>{target.module_base, target.module_name};
            if (std::ranges::find(hooked, key) == hooked.end()) {
                hooked.push_back(key);
            }
            if (current == target.replacement) {
                continue;
            }
            if (current != target.original) {
                error = "ELF import slot changed while applying hooks: " + target.module_name;
                return false;
            }
        }
        else {
            if (current != target.replacement) {
                continue;
            }
        }
        updates.push_back(&target);
    }
    if (replacements) {
        hooked_modules_ = hooked.size();
    }

    struct PatchPage {
        void *address = nullptr;
        int protection = 0;
    };
    const std::vector<MapRange> memory_map = readMemoryMap();
    std::vector<PatchPage> writable_pages;
    writable_pages.reserve(updates.size());
    for (const Target *target : updates) {
        const int protection = protectionForAddress(memory_map, reinterpret_cast<std::uintptr_t>(target->slot));
        if (protection < 0) {
            error = "ELF import slot became unmapped before patch: " + target->module_name;
            return false;
        }
        const std::uintptr_t page_address = reinterpret_cast<std::uintptr_t>(target->slot) & ~(page_size - 1);
        auto existing = std::ranges::find_if(writable_pages, [page_address](const PatchPage &page) {
            return reinterpret_cast<std::uintptr_t>(page.address) == page_address;
        });
        if (existing == writable_pages.end()) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            writable_pages.push_back({reinterpret_cast<void *>(page_address), protection});
        }
    }

    std::vector<PatchPage *> changed_pages;
    changed_pages.reserve(writable_pages.size());
    for (PatchPage &page : writable_pages) {
        if ((page.protection & PROT_WRITE) != 0) {
            continue;
        }
        if (::mprotect(page.address, static_cast<std::size_t>(page_size), page.protection | PROT_WRITE) != 0) {
            error = systemError("mprotect writable");
            for (PatchPage *changed : changed_pages) {
                ::mprotect(changed->address, static_cast<std::size_t>(page_size), changed->protection);
            }
            return false;
        }
        changed_pages.push_back(&page);
    }

    bool exchanged = true;
    for (const Target *target : updates) {
        if (before_write_gate_ != nullptr) {
            before_write_gate_(target->slot, replacements);
        }
        void *expected = replacements ? target->original : target->replacement;
        if (!__atomic_compare_exchange_n(target->slot, &expected, replacements ? target->replacement : target->original,
                                         false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
            replacements && expected != target->replacement) {
            exchanged = false;
            error = "ELF import slot changed immediately before patch: " + target->module_name;
        }
    }

    bool restored = true;
    for (PatchPage *page : changed_pages) {
        if (::mprotect(page->address, static_cast<std::size_t>(page_size), page->protection) != 0) {
            if (error.empty()) {
                error = systemError("mprotect restore");
            }
            restored = false;
        }
    }
    return restored && exchanged;
}

bool ElfImportHooks::install(std::string &error)
{
    if (!prepared_) {
        error = "ELF import hooks have not been prepared";
        return false;
    }
    if (installed_) {
        error.clear();
        return true;
    }
    if (!patch(true, error)) {
        std::string rollback_error;
        if (!patch(false, rollback_error)) {
            std::abort();
        }
        if (!rollback_error.empty()) {
            error += "; rollback: " + rollback_error;
        }
        return false;
    }
    installed_ = true;
    return true;
}

bool ElfImportHooks::uninstall(std::string &error)
{
    if (!installed_) {
        error.clear();
        return true;
    }
    if (!patch(false, error)) {
        return false;
    }
    installed_ = false;
    return true;
}

}  // namespace spark
