#include "native/alloc/elf_import_hooks.h"

#if !defined(__linux__) || !defined(__x86_64__)
#error "elf_import_hooks.cpp requires Linux x86-64"
#endif

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fstream>
#include <link.h>
#include <limits>
#include <sstream>
#include <string_view>
#include <sys/mman.h>
#include <type_traits>
#include <unistd.h>

namespace spark {
namespace {

constexpr std::size_t kMaxElfModules = 512;
constexpr std::size_t kMaxImportTargets = 65536;

struct Image {
    std::uintptr_t base = 0;
    std::uintptr_t load_begin = 0;
    std::uintptr_t load_end = 0;
    const ElfW(Phdr) *headers = nullptr;
    ElfW(Half) header_count = 0;
    std::string name;
    bool main_executable = false;
};

std::string executablePath()
{
    char path[4096]{};
    const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    return length > 0 ? std::string(path, static_cast<std::size_t>(length))
                      : std::string("<main executable>");
}

int collectImages(dl_phdr_info *info, std::size_t, void *opaque)
{
    auto &images = *static_cast<std::vector<Image> *>(opaque);
    if (images.size() == kMaxElfModules) {
        return 1;
    }

    Image image;
    image.base = static_cast<std::uintptr_t>(info->dlpi_addr);
    image.headers = info->dlpi_phdr;
    image.header_count = info->dlpi_phnum;
    image.main_executable =
        info->dlpi_name == nullptr || info->dlpi_name[0] == '\0';
    image.name = info->dlpi_name != nullptr && info->dlpi_name[0] != '\0'
                     ? std::string(info->dlpi_name)
                     : executablePath();
    image.load_begin = (std::numeric_limits<std::uintptr_t>::max)();
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD) {
            continue;
        }
        image.load_begin =
            (std::min)(image.load_begin,
                       image.base + static_cast<std::uintptr_t>(header.p_vaddr));
        image.load_end =
            (std::max)(image.load_end,
                       image.base +
                           static_cast<std::uintptr_t>(header.p_vaddr + header.p_memsz));
    }
    if (image.load_begin != (std::numeric_limits<std::uintptr_t>::max)() &&
        image.load_end > image.load_begin) {
        images.push_back(std::move(image));
    }
    return 0;
}

bool contains(const Image &image, std::uintptr_t address,
              std::size_t bytes = 1) noexcept
{
    return address >= image.load_begin && address < image.load_end &&
           bytes <= image.load_end - address;
}

std::uintptr_t dynamicPointer(const Image &image, ElfW(Addr) value) noexcept
{
    const auto address = static_cast<std::uintptr_t>(value);
    return contains(image, address) ? address : image.base + address;
}

bool supportedRelocation(unsigned type) noexcept
{
    return type == R_X86_64_JUMP_SLOT || type == R_X86_64_GLOB_DAT ||
           type == R_X86_64_64;
}

std::string basename(std::string_view path)
{
    const std::size_t separator = path.find_last_of('/');
    return std::string(separator == std::string_view::npos
                           ? path
                           : path.substr(separator + 1));
}

bool isSparkImage(const Image &image, std::uintptr_t replacement_base)
{
    if (image.base != replacement_base) {
        return false;
    }
    const std::string name = basename(image.name);
    return name.find("endstone_spark") != std::string::npos ||
           name == "spark.so" || name == "libspark.so";
}

bool isLoaderImage(std::string_view path)
{
    return path.find("linux-vdso") != std::string_view::npos ||
           path.find("ld-linux") != std::string_view::npos ||
           path.find("/ld-") != std::string_view::npos;
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
        mapped.protection |= permissions.size() > 0 && permissions[0] == 'r'
                                 ? PROT_READ
                                 : 0;
        mapped.protection |= permissions.size() > 1 && permissions[1] == 'w'
                                 ? PROT_WRITE
                                 : 0;
        mapped.protection |= permissions.size() > 2 && permissions[2] == 'x'
                                 ? PROT_EXEC
                                 : 0;
        ranges.push_back(mapped);
    }
    return ranges;
}

int protectionForAddress(const std::vector<MapRange> &ranges,
                         std::uintptr_t address) noexcept
{
    auto found = std::find_if(ranges.begin(), ranges.end(),
                              [address](const MapRange &range) {
                                  return address >= range.begin &&
                                         address < range.end;
                              });
    return found == ranges.end() ? -1 : found->protection;
}

std::string systemError(const char *operation)
{
    return std::string(operation) + " failed: " + std::strerror(errno);
}

bool sameModule(const Image &image, std::uintptr_t base,
                const std::string &name)
{
    return image.base == base && image.name == name;
}

}  // namespace

bool ElfImportHooks::prepare(std::span<const ElfImportHookSpec> specs,
                             std::string &error)
{
    error.clear();
    if (prepared_) {
        return rescan(error);
    }
    if (specs.empty()) {
        error = "no ELF import hooks were requested";
        return false;
    }

    specs_.assign(specs.begin(), specs.end());
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
    std::vector<Image> images;
    images.reserve(64);
    ::dl_iterate_phdr(collectImages, &images);
    if (images.empty()) {
        error = "could not enumerate loaded Linux ELF images";
        return false;
    }

    std::uintptr_t replacement_base = 0;
    if (!specs_.empty() && specs_.front().replacement != nullptr) {
        Dl_info replacement{};
        if (::dladdr(specs_.front().replacement, &replacement) != 0) {
            replacement_base =
                reinterpret_cast<std::uintptr_t>(replacement.dli_fbase);
        }
    }

    std::vector<std::uintptr_t> allocator_bases;
    for (const ElfImportHookSpec &spec : specs_) {
        if (spec.name == nullptr) {
            continue;
        }
        void *address = ::dlsym(RTLD_DEFAULT, spec.name);
        Dl_info owner{};
        if (address != nullptr && ::dladdr(address, &owner) != 0) {
            allocator_bases.push_back(
                reinterpret_cast<std::uintptr_t>(owner.dli_fbase));
        }
    }
    std::sort(allocator_bases.begin(), allocator_bases.end());
    allocator_bases.erase(
        std::unique(allocator_bases.begin(), allocator_bases.end()),
        allocator_bases.end());

    const std::vector<MapRange> memory_map = readMemoryMap();
    skipped_modules_ = 0;
    failed_modules_ = 0;
    std::vector<std::pair<std::uintptr_t, std::string>> failed_modules;
    auto markFailed = [&](std::uintptr_t base, const std::string &name) {
        const auto key = std::pair<std::uintptr_t, std::string>{base, name};
        if (std::find(failed_modules.begin(), failed_modules.end(), key) ==
            failed_modules.end()) {
            failed_modules.push_back(key);
            failed_modules_ = failed_modules.size();
        }
    };

    for (const Image &image : images) {
        const bool already_hooked = std::any_of(
            targets_.begin(), targets_.end(), [&image](const Target &target) {
                return target.module_base == image.base &&
                       target.module_name == image.name;
            });
        if (isSparkImage(image, replacement_base) ||
            std::binary_search(allocator_bases.begin(), allocator_bases.end(),
                               image.base) ||
            isLoaderImage(image.name)) {
            ++skipped_modules_;
            continue;
        }

        const ElfW(Dyn) *dynamic = nullptr;
        for (ElfW(Half) i = 0; i < image.header_count; ++i) {
            if (image.headers[i].p_type == PT_DYNAMIC) {
                const std::uintptr_t address =
                    image.base +
                    static_cast<std::uintptr_t>(image.headers[i].p_vaddr);
                if (contains(image, address, sizeof(ElfW(Dyn)))) {
                    dynamic = reinterpret_cast<const ElfW(Dyn) *>(address);
                }
                break;
            }
        }
        if (dynamic == nullptr) {
            ++skipped_modules_;
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
            (image.load_end - reinterpret_cast<std::uintptr_t>(dynamic)) /
            sizeof(ElfW(Dyn));
        for (std::size_t i = 0; i < max_dynamic; ++i) {
            const ElfW(Dyn) &entry = dynamic[i];
            if (entry.d_tag == DT_NULL) {
                dynamic_valid = true;
                break;
            }
            switch (entry.d_tag) {
            case DT_SYMTAB:
                symbols = reinterpret_cast<const ElfW(Sym) *>(
                    dynamicPointer(image, entry.d_un.d_ptr));
                break;
            case DT_STRTAB:
                strings = reinterpret_cast<const char *>(
                    dynamicPointer(image, entry.d_un.d_ptr));
                break;
            case DT_STRSZ:
                string_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_SYMENT:
                symbol_entry_size =
                    static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_REL:
                rel = reinterpret_cast<const ElfW(Rel) *>(
                    dynamicPointer(image, entry.d_un.d_ptr));
                break;
            case DT_RELSZ:
                rel_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_RELENT:
                rel_entry_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_RELA:
                rela = reinterpret_cast<const ElfW(Rela) *>(
                    dynamicPointer(image, entry.d_un.d_ptr));
                break;
            case DT_RELASZ:
                rela_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_RELAENT:
                rela_entry_size = static_cast<std::size_t>(entry.d_un.d_val);
                break;
            case DT_JMPREL:
                jmprel = reinterpret_cast<const void *>(
                    dynamicPointer(image, entry.d_un.d_ptr));
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
        if (!dynamic_valid || symbols == nullptr || strings == nullptr ||
            string_size == 0 || symbol_entry_size != sizeof(ElfW(Sym)) ||
            !contains(image, reinterpret_cast<std::uintptr_t>(symbols),
                      sizeof(ElfW(Sym))) ||
            !contains(image, reinterpret_cast<std::uintptr_t>(strings),
                      string_size) ||
            (rel != nullptr && rel_entry_size != sizeof(ElfW(Rel))) ||
            (rela != nullptr && rela_entry_size != sizeof(ElfW(Rela)))) {
            markFailed(image.base, image.name);
            continue;
        }

        const std::size_t before = targets_.size();
        auto visit = [&](auto *entries, std::size_t bytes) {
            using Relocation = std::remove_cv_t<
                std::remove_pointer_t<decltype(entries)>>;
            const std::uintptr_t entries_address =
                reinterpret_cast<std::uintptr_t>(entries);
            if (entries == nullptr || bytes % sizeof(Relocation) != 0 ||
                !contains(image, entries_address, bytes)) {
                return;
            }
            const std::size_t count = bytes / sizeof(Relocation);
            for (std::size_t i = 0; i < count; ++i) {
                const Relocation &relocation = entries[i];
                const unsigned type =
                    static_cast<unsigned>(ELF64_R_TYPE(relocation.r_info));
                if (!supportedRelocation(type)) {
                    continue;
                }
                const std::size_t symbol_index =
                    static_cast<std::size_t>(ELF64_R_SYM(relocation.r_info));
                const std::uintptr_t symbols_address =
                    reinterpret_cast<std::uintptr_t>(symbols);
                if (symbol_index >
                    ((std::numeric_limits<std::uintptr_t>::max)() -
                     symbols_address) /
                        sizeof(ElfW(Sym))) {
                    continue;
                }
                const std::uintptr_t symbol_address =
                    symbols_address +
                    symbol_index * sizeof(ElfW(Sym));
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
                for (std::size_t spec_index = 0; spec_index < specs_.size();
                     ++spec_index) {
                    const ElfImportHookSpec &spec = specs_[spec_index];
                    if (spec.name == nullptr ||
                        std::strcmp(name, spec.name) != 0) {
                        continue;
                    }
                    const std::uintptr_t slot_address =
                        image.base +
                        static_cast<std::uintptr_t>(relocation.r_offset);
                    if (!contains(image, slot_address, sizeof(void *)) ||
                        slot_address % alignof(void *) != 0) {
                        continue;
                    }
                    auto **slot = reinterpret_cast<void **>(slot_address);
                    auto existing = std::find_if(
                        targets_.begin(), targets_.end(),
                        [slot, &image](const Target &target) {
                            return target.slot == slot &&
                                   target.module_base == image.base &&
                                   target.module_name == image.name;
                        });
                    if (existing != targets_.end()) {
                        void *current =
                            __atomic_load_n(slot, __ATOMIC_ACQUIRE);
                        if (current != existing->replacement) {
                            existing->original = current;
                        }
                        continue;
                    }
                    if (targets_.size() == kMaxImportTargets) {
                        continue;
                    }
                    targets_.push_back(
                        {slot, __atomic_load_n(slot, __ATOMIC_ACQUIRE),
                         spec.replacement, spec_index, image.base, image.name});
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
            markFailed(image.base, image.name);
        }

        if (targets_.size() == before && !already_hooked) {
            ++skipped_modules_;
        }
    }

    const long page_size_value = ::sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) {
        error = "sysconf(_SC_PAGESIZE) failed";
        return false;
    }
    const std::uintptr_t page_size =
        static_cast<std::uintptr_t>(page_size_value);
    std::vector<std::uintptr_t> failed_pages;
    for (const Target &target : targets_) {
        const std::uintptr_t page_address =
            reinterpret_cast<std::uintptr_t>(target.slot) & ~(page_size - 1);
        auto existing_page = std::find_if(
            pages_.begin(), pages_.end(),
            [page_address, &target](const Page &page) {
                return reinterpret_cast<std::uintptr_t>(page.address) ==
                           page_address &&
                       page.module_base == target.module_base &&
                       page.module_name == target.module_name;
            });
        const int protection =
            protectionForAddress(memory_map,
                                 reinterpret_cast<std::uintptr_t>(target.slot));
        if (protection < 0) {
            markFailed(target.module_base, target.module_name);
            failed_pages.push_back(page_address);
            continue;
        }
        if (existing_page == pages_.end()) {
            pages_.push_back({reinterpret_cast<void *>(page_address), protection,
                              target.module_base, target.module_name});
        }
        else {
            existing_page->protection = protection;
        }
    }
    if (!failed_pages.empty()) {
        targets_.erase(
            std::remove_if(
                targets_.begin(), targets_.end(),
                [&failed_pages, page_size](const Target &target) {
                    const std::uintptr_t page =
                        reinterpret_cast<std::uintptr_t>(target.slot) &
                        ~(page_size - 1);
                    return std::find(failed_pages.begin(), failed_pages.end(),
                                     page) != failed_pages.end();
                }),
            targets_.end());
    }

    capabilities_.clear();
    capabilities_.reserve(specs_.size());
    for (std::size_t i = 0; i < specs_.size(); ++i) {
        ElfImportHookCapability capability;
        capability.name = specs_[i].name != nullptr ? specs_[i].name : "";
        capability.slots = static_cast<std::size_t>(std::count_if(
            targets_.begin(), targets_.end(),
            [i](const Target &target) { return target.spec_index == i; }));
        capability.available = capability.slots != 0;
        if (!capability.available) {
            capability.detail = "import not found in supported loaded modules";
            if (specs_[i].required) {
                error = "required Linux allocator import not found: " +
                        capability.name;
                return false;
            }
        }
        capabilities_.push_back(std::move(capability));
    }

    std::vector<std::pair<std::uintptr_t, std::string_view>> hooked;
    for (const Target &target : targets_) {
        const bool loaded = std::any_of(
            images.begin(), images.end(), [&target](const Image &image) {
                return sameModule(image, target.module_base, target.module_name);
            });
        if (!loaded) {
            continue;
        }
        const auto key =
            std::pair<std::uintptr_t, std::string_view>{target.module_base,
                                                        target.module_name};
        if (std::find(hooked.begin(), hooked.end(), key) == hooked.end()) {
            hooked.push_back(key);
        }
    }
    hooked_modules_ = hooked.size();
    return true;
}

bool ElfImportHooks::patch(bool replacements, std::string &error)
{
    error.clear();
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        error = "sysconf(_SC_PAGESIZE) failed";
        return false;
    }

    std::vector<Image> images;
    images.reserve(64);
    ::dl_iterate_phdr(collectImages, &images);

    struct PinnedImage {
        std::uintptr_t base = 0;
        std::string name;
        void *handle = nullptr;
    };
    std::vector<PinnedImage> pinned;
    pinned.reserve(images.size());
    for (const Image &image : images) {
        const bool referenced =
            std::any_of(pages_.begin(), pages_.end(),
                        [&image](const Page &page) {
                            return sameModule(image, page.module_base,
                                              page.module_name);
                        }) ||
            std::any_of(targets_.begin(), targets_.end(),
                        [&image](const Target &target) {
                            return sameModule(image, target.module_base,
                                              target.module_name);
                        });
        if (!referenced) {
            continue;
        }
        void *handle =
            image.main_executable
                ? ::dlopen(nullptr, RTLD_NOW)
                : ::dlopen(image.name.c_str(), RTLD_NOW | RTLD_NOLOAD);
        if (handle != nullptr) {
            pinned.push_back({image.base, image.name, handle});
        }
    }
    auto loaded = [&pinned](std::uintptr_t base, const std::string &name) {
        return std::any_of(pinned.begin(), pinned.end(),
                           [base, &name](const PinnedImage &image) {
                               return image.base == base && image.name == name;
                           });
    };
    if (replacements) {
        std::vector<std::pair<std::uintptr_t, std::string_view>> hooked;
        for (const Target &target : targets_) {
            if (!loaded(target.module_base, target.module_name)) {
                continue;
            }
            const auto key = std::pair<std::uintptr_t, std::string_view>{
                target.module_base, target.module_name};
            if (std::find(hooked.begin(), hooked.end(), key) == hooked.end()) {
                hooked.push_back(key);
            }
        }
        hooked_modules_ = hooked.size();
    }

    std::vector<const Page *> writable;
    writable.reserve(pages_.size());
    for (const Page &page : pages_) {
        if (!loaded(page.module_base, page.module_name)) {
            continue;
        }
        if ((page.protection & PROT_WRITE) == 0 &&
            ::mprotect(page.address, static_cast<std::size_t>(page_size),
                       page.protection | PROT_WRITE) != 0) {
            error = systemError("mprotect writable");
            break;
        }
        writable.push_back(&page);
    }
    if (!error.empty()) {
        for (const Page *page : writable) {
            if ((page->protection & PROT_WRITE) == 0) {
                ::mprotect(page->address, static_cast<std::size_t>(page_size),
                           page->protection);
            }
        }
        for (const PinnedImage &image : pinned) {
            ::dlclose(image.handle);
        }
        return false;
    }

    for (const Target &target : targets_) {
        if (loaded(target.module_base, target.module_name)) {
            __atomic_store_n(target.slot,
                             replacements ? target.replacement : target.original,
                             __ATOMIC_RELEASE);
        }
    }

    bool restored = true;
    for (const Page *page : writable) {
        if ((page->protection & PROT_WRITE) == 0 &&
            ::mprotect(page->address, static_cast<std::size_t>(page_size),
                       page->protection) != 0) {
            if (error.empty()) {
                error = systemError("mprotect restore");
            }
            restored = false;
        }
    }
    for (const PinnedImage &image : pinned) {
        ::dlclose(image.handle);
    }
    return restored;
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
