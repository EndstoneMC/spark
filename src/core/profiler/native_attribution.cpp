#include "core/profiler/native_attribution.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <type_traits>

#if defined(__linux__) && defined(__x86_64__)
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#endif

#include "native/sampler/call_tree.h"
#include "proto/sampler_data.h"

namespace spark {
namespace {

template <std::size_t N>
bool matches(std::string_view method_name, const std::array<std::string_view, N> &known_methods) noexcept
{
    return std::ranges::any_of(
        known_methods, [method_name](const std::string_view known_method) { return method_name == known_method; });
}

constexpr std::array KLinuxMethods{
    std::string_view("spark::AllocationSampler::Impl::hookMalloc(void*, unsigned long)"),
    std::string_view("spark::AllocationSampler::Impl::hookCalloc(void*, unsigned long, unsigned long)"),
    std::string_view("spark::AllocationSampler::Impl::hookRealloc(void*, void*, unsigned long)"),
    std::string_view("spark::AllocationSampler::Impl::hookFree(void*, void*)"),
    std::string_view("spark::AllocationSampler::Impl::hookReallocArray(void*, void*, unsigned long, unsigned long)"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedAlloc(void*, unsigned long, unsigned long)"),
    std::string_view("spark::AllocationSampler::Impl::hookPosixMemalign(void*, void**, unsigned long, unsigned long)"),
};

constexpr std::array KItaniumMethods{
    std::string_view("_ZN5spark17AllocationSampler4Impl10hookMallocEPvm"),
    std::string_view("_ZN5spark17AllocationSampler4Impl10hookCallocEPvmm"),
    std::string_view("_ZN5spark17AllocationSampler4Impl11hookReallocEPvS2_m"),
    std::string_view("_ZN5spark17AllocationSampler4Impl8hookFreeEPvS2_"),
    std::string_view("_ZN5spark17AllocationSampler4Impl16hookReallocArrayEPvS2_mm"),
    std::string_view("_ZN5spark17AllocationSampler4Impl16hookAlignedAllocEPvmm"),
    std::string_view("_ZN5spark17AllocationSampler4Impl17hookPosixMemalignEPvPS2_mm"),
};

constexpr std::array KSignaturelessMethods{
    std::string_view("spark::AllocationSampler::Impl::hookMalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookCalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookRealloc"),
    std::string_view("spark::AllocationSampler::Impl::hookRecalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookFree"),
    std::string_view("spark::AllocationSampler::Impl::hookReallocArray"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedAlloc"),
    std::string_view("spark::AllocationSampler::Impl::hookPosixMemalign"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedMalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedRealloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedRecalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedOffsetMalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedOffsetRealloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedOffsetRecalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedFree"),
    std::string_view("spark::AllocationSampler::Impl::hookMallocBase"),
    std::string_view("spark::AllocationSampler::Impl::hookCallocBase"),
    std::string_view("spark::AllocationSampler::Impl::hookReallocBase"),
    std::string_view("spark::AllocationSampler::Impl::hookFreeBase"),
    std::string_view("spark::AllocationSampler::Impl::hookHeapAlloc"),
    std::string_view("spark::AllocationSampler::Impl::hookHeapReAlloc"),
    std::string_view("spark::AllocationSampler::Impl::hookHeapFree"),
};

constexpr std::array KPythonAttributionObserverMethods{
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyStartThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyResumeThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyThrowThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyReturnThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyYieldThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyUnwindThunk"),
};

constexpr std::array KPythonAttributionBridgeMethods{
    std::string_view("_ctypes_callproc"), std::string_view("_call_function_pointer"),
    std::string_view("ffi_call"),         std::string_view("ffi_call_int"),
    std::string_view("ffi_call_unix64"),  std::string_view("ffi_call_win64"),
};

bool matchesFunction(std::string_view method_name, std::string_view known_method) noexcept
{
    return method_name == known_method ||
           (method_name.size() > known_method.size() && method_name.starts_with(known_method) &&
            method_name[known_method.size()] == '(');
}

template <std::size_t N>
bool matchesFunction(std::string_view method_name, const std::array<std::string_view, N> &known_methods) noexcept
{
    return std::ranges::any_of(known_methods, [method_name](std::string_view known_method) {
        return matchesFunction(method_name, known_method);
    });
}

bool isPythonAttributionObserver(std::string_view method_name) noexcept
{
    return matchesFunction(method_name, KPythonAttributionObserverMethods);
}

bool isPythonAttributionBridge(const FrameKey &key, const ResolvedFrameMap &resolved) noexcept
{
    const auto frame = resolved.find(key);
    return frame != resolved.end() && matchesFunction(frame->second.method_name, KPythonAttributionBridgeMethods);
}

bool isHexDigit(char value) noexcept
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

bool isUnresolvedMethod(std::string_view method_name) noexcept
{
    return method_name.size() > 2 && method_name.starts_with("0x") &&
           std::ranges::all_of(method_name.substr(2), isHexDigit);
}

#if defined(__linux__) && defined(__x86_64__)

constexpr std::size_t KMaxDiscoveredHookTargets = 32;
constexpr std::size_t KAllocatorImportCount = 7;

struct LinuxDiscoveryContext {
    std::uintptr_t self_base = 0;
    std::uintptr_t self_exec_begin = (std::numeric_limits<std::uintptr_t>::max)();
    std::uintptr_t self_exec_end = 0;
    std::vector<std::uintptr_t> targets;
};

struct LinuxImageView {
    std::uintptr_t base = 0;
    std::uintptr_t load_begin = (std::numeric_limits<std::uintptr_t>::max)();
    std::uintptr_t load_end = 0;
    const ElfW(Phdr) *headers = nullptr;
    std::size_t header_count = 0;
};

struct LinuxRelocationView {
    const LinuxImageView &image;
    const ElfW(Sym) *symbols = nullptr;
    const char *strings;
    std::size_t string_size;
    LinuxDiscoveryContext &context;
};

bool populateImageView(const dl_phdr_info &info, LinuxImageView &image) noexcept
{
    image.base = static_cast<std::uintptr_t>(info.dlpi_addr);
    image.headers = info.dlpi_phdr;
    image.header_count = info.dlpi_phnum;
    for (std::size_t i = 0; i < image.header_count; ++i) {
        const ElfW(Phdr) &header = image.headers[i];
        if (header.p_type != PT_LOAD) {
            continue;
        }
        image.load_begin = (std::min)(image.load_begin, image.base + static_cast<std::uintptr_t>(header.p_vaddr));
        image.load_end =
            (std::max)(image.load_end, image.base + static_cast<std::uintptr_t>(header.p_vaddr + header.p_memsz));
    }
    return image.load_begin != (std::numeric_limits<std::uintptr_t>::max)() && image.load_end > image.load_begin;
}

bool contains(const LinuxImageView &image, std::uintptr_t address, std::size_t bytes = 1) noexcept
{
    return address >= image.load_begin && address < image.load_end && bytes <= image.load_end - address;
}

std::uintptr_t dynamicPointer(const LinuxImageView &image, ElfW(Addr) value) noexcept
{
    const auto address = static_cast<std::uintptr_t>(value);
    return contains(image, address) ? address : image.base + address;
}

bool supportedRelocation(unsigned type) noexcept
{
    return type == R_X86_64_JUMP_SLOT || type == R_X86_64_GLOB_DAT || type == R_X86_64_64;
}

bool allocatorImport(std::string_view name) noexcept
{
    if (name == "malloc") {
        return true;
    }
    if (name == "calloc") {
        return true;
    }
    if (name == "realloc") {
        return true;
    }
    if (name == "free") {
        return true;
    }
    if (name == "reallocarray") {
        return true;
    }
    if (name == "aligned_alloc") {
        return true;
    }
    return name == "posix_memalign";
}

int findSelfExecutableRange(dl_phdr_info *info, std::size_t, void *opaque)
{
    auto &context = *static_cast<LinuxDiscoveryContext *>(opaque);
    if (static_cast<std::uintptr_t>(info->dlpi_addr) != context.self_base) {
        return 0;
    }
    for (std::size_t i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) {
            continue;
        }
        context.self_exec_begin =
            (std::min)(context.self_exec_begin, context.self_base + static_cast<std::uintptr_t>(header.p_vaddr));
        context.self_exec_end =
            (std::max)(context.self_exec_end,
                       context.self_base + static_cast<std::uintptr_t>(header.p_vaddr + header.p_memsz));
    }
    return 1;
}

template <typename Relocation>
void visitRelocations(const Relocation *entries, std::size_t bytes, const LinuxRelocationView &view)
{
    const LinuxImageView &image = view.image;
    const ElfW(Sym) *symbols = view.symbols;
    const char *strings = view.strings;
    const std::size_t string_size = view.string_size;
    LinuxDiscoveryContext &context = view.context;
    const auto entries_address = reinterpret_cast<std::uintptr_t>(entries);
    if (entries == nullptr || bytes % sizeof(Relocation) != 0 || !contains(image, entries_address, bytes)) {
        return;
    }
    const std::size_t count = bytes / sizeof(Relocation);
    const auto symbols_address = reinterpret_cast<std::uintptr_t>(symbols);
    for (std::size_t i = 0; i < count; ++i) {
        const Relocation &relocation = entries[i];
        if (!supportedRelocation(static_cast<unsigned>(ELF64_R_TYPE(relocation.r_info)))) {
            continue;
        }
        const auto symbol_index = static_cast<std::size_t>(ELF64_R_SYM(relocation.r_info));
        if (symbol_index > ((std::numeric_limits<std::uintptr_t>::max)() - symbols_address) / sizeof(ElfW(Sym))) {
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
        if (std::memchr(name, '\0', remaining) == nullptr || !allocatorImport(std::string_view(name))) {
            continue;
        }
        const std::uintptr_t slot_address = image.base + static_cast<std::uintptr_t>(relocation.r_offset);
        if (!contains(image, slot_address, sizeof(void *)) || slot_address % alignof(void *) != 0) {
            continue;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto **slot = reinterpret_cast<void **>(slot_address);
        const auto target = reinterpret_cast<std::uintptr_t>(__atomic_load_n(slot, __ATOMIC_ACQUIRE));
        if (target < context.self_exec_begin || target >= context.self_exec_end ||
            std::ranges::find(context.targets, target) != context.targets.end() ||
            context.targets.size() == KMaxDiscoveredHookTargets) {
            continue;
        }
        context.targets.push_back(target);
    }
}

int collectHookTargets(dl_phdr_info *info, std::size_t, void *opaque)
{
    auto &context = *static_cast<LinuxDiscoveryContext *>(opaque);
    LinuxImageView image;
    if (!populateImageView(*info, image)) {
        return 0;
    }

    const ElfW(Dyn) *dynamic = nullptr;
    for (std::size_t i = 0; i < image.header_count; ++i) {
        const ElfW(Phdr) &header = image.headers[i];
        if (header.p_type != PT_DYNAMIC) {
            continue;
        }
        const std::uintptr_t address = image.base + static_cast<std::uintptr_t>(header.p_vaddr);
        if (contains(image, address, sizeof(ElfW(Dyn)))) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            dynamic = reinterpret_cast<const ElfW(Dyn) *>(address);
        }
        break;
    }
    if (dynamic == nullptr) {
        return 0;
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
    const auto dynamic_address = reinterpret_cast<std::uintptr_t>(dynamic);
    const std::size_t max_dynamic = (image.load_end - dynamic_address) / sizeof(ElfW(Dyn));
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
        return 0;
    }

    const LinuxRelocationView relocation_view{
        .image = image,
        .symbols = symbols,
        .strings = strings,
        .string_size = string_size,
        .context = context,
    };
    visitRelocations(rel, rel_size, relocation_view);
    visitRelocations(rela, rela_size, relocation_view);
    if (plt_type == DT_REL) {
        visitRelocations(static_cast<const ElfW(Rel) *>(jmprel), jmprel_size, relocation_view);
    }
    else if (plt_type == DT_RELA) {
        visitRelocations(static_cast<const ElfW(Rela) *>(jmprel), jmprel_size, relocation_view);
    }
    return context.targets.size() == KMaxDiscoveredHookTargets ? 1 : 0;
}

struct DwarfEhBases {
    void *tbase = nullptr;
    void *dbase = nullptr;
    void *func = nullptr;
};

using FindFdeFunction = const void *(*)(void *, DwarfEhBases *);

void *addressPointer(std::uintptr_t address) noexcept
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void *>(address);
}

std::optional<NativeInstrumentationRange> unwindFunctionRange(std::uintptr_t target, std::uintptr_t executable_begin,
                                                              std::uintptr_t executable_end, FindFdeFunction find_fde)
{
    DwarfEhBases bases{};
    const void *fde = find_fde(addressPointer(target), &bases);
    if (fde == nullptr || bases.func == nullptr) {
        return std::nullopt;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(bases.func);
    if (begin < executable_begin || begin > target || target >= executable_end) {
        return std::nullopt;
    }

    auto same_fde = [find_fde, fde](std::uintptr_t address) {
        DwarfEhBases probe{};
        return find_fde(addressPointer(address), &probe) == fde;
    };
    if (!same_fde(begin)) {
        return std::nullopt;
    }
    if (target + 1 >= executable_end) {
        return NativeInstrumentationRange{.begin = begin, .end = executable_end};
    }

    std::uintptr_t last_same = target;
    std::uintptr_t first_other = executable_end;
    std::uintptr_t step = 32;
    while (last_same + 1 < executable_end) {
        const std::uintptr_t maximum_delta = executable_end - 1 - target;
        const std::uintptr_t delta = (std::min)(step, maximum_delta);
        const std::uintptr_t candidate = target + delta;
        if (candidate <= last_same) {
            break;
        }
        if (!same_fde(candidate)) {
            first_other = candidate;
            break;
        }
        last_same = candidate;
        if (candidate == executable_end - 1) {
            break;
        }
        step = step > maximum_delta / 2 ? maximum_delta : step * 2;
    }

    if (last_same == executable_end - 1) {
        return NativeInstrumentationRange{.begin = begin, .end = executable_end};
    }
    if (first_other == executable_end) {
        return std::nullopt;
    }

    std::uintptr_t low = last_same + 1;
    std::uintptr_t high = first_other;
    while (low < high) {
        const std::uintptr_t middle = low + (high - low) / 2;
        if (same_fde(middle)) {
            low = middle + 1;
        }
        else {
            high = middle;
        }
    }
    if (low <= begin || low <= target) {
        return std::nullopt;
    }
    return NativeInstrumentationRange{.begin = begin, .end = low};
}

std::vector<NativeInstrumentationRange> discoverAllocationInstrumentationRanges()
{
    LinuxDiscoveryContext context;
    Dl_info self{};
    if (::dladdr(static_cast<const void *>(&KLinuxMethods), &self) == 0 || self.dli_fbase == nullptr) {
        return {};
    }
    context.self_base = reinterpret_cast<std::uintptr_t>(self.dli_fbase);
    ::dl_iterate_phdr(findSelfExecutableRange, &context);
    if (context.self_exec_begin == (std::numeric_limits<std::uintptr_t>::max)() ||
        context.self_exec_end <= context.self_exec_begin) {
        return {};
    }
    context.targets.reserve(KAllocatorImportCount);
    ::dl_iterate_phdr(collectHookTargets, &context);
    if (context.targets.empty()) {
        return {};
    }

    void *unwind_handle = nullptr;
    void *find_fde_symbol = ::dlsym(RTLD_DEFAULT, "_Unwind_Find_FDE");
    if (find_fde_symbol == nullptr) {
        unwind_handle = ::dlopen("libgcc_s.so.1", RTLD_NOW | RTLD_LOCAL);
        if (unwind_handle != nullptr) {
            find_fde_symbol = ::dlsym(unwind_handle, "_Unwind_Find_FDE");
        }
    }
    if (find_fde_symbol == nullptr) {
        if (unwind_handle != nullptr) {
            ::dlclose(unwind_handle);
        }
        return {};
    }
    const auto find_fde = reinterpret_cast<FindFdeFunction>(find_fde_symbol);

    std::vector<NativeInstrumentationRange> ranges;
    ranges.reserve(context.targets.size());
    for (const std::uintptr_t target : context.targets) {
        const auto range = unwindFunctionRange(target, context.self_exec_begin, context.self_exec_end, find_fde);
        if (!range.has_value()) {
            continue;
        }
        const bool duplicate = std::ranges::any_of(ranges, [&range](const NativeInstrumentationRange &existing) {
            return existing.begin == range->begin && existing.end == range->end;
        });
        if (!duplicate) {
            ranges.push_back(*range);
        }
    }
    if (unwind_handle != nullptr) {
        ::dlclose(unwind_handle);
    }
    std::ranges::sort(ranges, {}, &NativeInstrumentationRange::begin);
    return ranges;
}

#else

std::vector<NativeInstrumentationRange> discoverAllocationInstrumentationRanges()
{
    return {};
}

#endif

enum class NodeResult {
    Retained,
    Dropped,
    ObserverDropped,
    Malformed,
};

bool addCounts(std::map<std::int32_t, std::uint64_t> &totals, const std::map<std::int32_t, std::uint64_t> &counts)
{
    for (const auto &[window, count] : counts) {
        auto &total = totals[window];
        if (count > std::numeric_limits<std::uint64_t>::max() - total) {
            return false;
        }
        total += count;
    }
    return true;
}

NodeResult instrumentationResult(const FrameKey &key, const ResolvedFrameMap &resolved,
                                 std::span<const NativeInstrumentationRange> instrumentation_ranges)
{
    const auto frame = resolved.find(key);
    if (frame != resolved.end() && isPythonAttributionObserver(frame->second.method_name)) {
        return NodeResult::ObserverDropped;
    }

    const bool exact_name = frame != resolved.end() && isNativeAllocationInstrumentation(frame->second.method_name);
    if (instrumentation_ranges.empty()) {
        return exact_name ? NodeResult::Dropped : NodeResult::Retained;
    }
    if (!isNativeAllocationInstrumentationAddress(key.raw_address, instrumentation_ranges)) {
        return NodeResult::Retained;
    }
    if (frame == resolved.end() || frame->second.method_name.empty() || isUnresolvedMethod(frame->second.method_name)) {
        return NodeResult::Dropped;
    }
    return exact_name ? NodeResult::Dropped : NodeResult::Retained;
}

// NOLINTNEXTLINE(misc-no-recursion)
NodeResult copyFilteredNode(const CallTree::Node &source, CallTree::Node &destination, const ResolvedFrameMap &resolved,
                            std::span<const NativeInstrumentationRange> instrumentation_ranges)
{
    const NodeResult instrumentation = instrumentationResult(source.key, resolved, instrumentation_ranges);
    if (instrumentation != NodeResult::Retained) {
        return instrumentation;
    }

    destination.key = source.key;
    bool observer_child_dropped = false;
    std::map<std::int32_t, std::uint64_t> original_child_totals;
    std::map<std::int32_t, std::uint64_t> retained_child_totals;
    for (const auto &[key, child] : source.children) {
        if (!addCounts(original_child_totals, child->times)) {
            return NodeResult::Malformed;
        }

        auto filtered_child = std::make_unique<CallTree::Node>();
        const NodeResult result = copyFilteredNode(*child, *filtered_child, resolved, instrumentation_ranges);
        if (result == NodeResult::Malformed) {
            return result;
        }
        if (result != NodeResult::Retained) {
            observer_child_dropped = observer_child_dropped || result == NodeResult::ObserverDropped;
            continue;
        }
        if (!addCounts(retained_child_totals, filtered_child->times)) {
            return NodeResult::Malformed;
        }
        destination.children.emplace(key, std::move(filtered_child));
    }

    if (observer_child_dropped && destination.children.empty() && isPythonAttributionBridge(source.key, resolved)) {
        return NodeResult::ObserverDropped;
    }

    std::map<std::int32_t, bool> windows;
    for (const auto &[window, count] : source.times) {
        (void)count;
        windows.emplace(window, false);
    }
    for (const auto &[window, count] : original_child_totals) {
        (void)count;
        windows.emplace(window, false);
    }
    for (const auto &[window, count] : retained_child_totals) {
        (void)count;
        windows.emplace(window, false);
    }

    for (const auto &[window, unused] : windows) {
        (void)unused;
        const auto source_count = source.times.find(window);
        const auto original_children = original_child_totals.find(window);
        const auto retained_children = retained_child_totals.find(window);
        const std::uint64_t total = source_count == source.times.end() ? 0 : source_count->second;
        const std::uint64_t child_total =
            original_children == original_child_totals.end() ? 0 : original_children->second;
        const std::uint64_t retained_total =
            retained_children == retained_child_totals.end() ? 0 : retained_children->second;
        if (child_total > total || retained_total > child_total) {
            return NodeResult::Malformed;
        }
        const std::uint64_t self = total - child_total;
        const std::uint64_t filtered_total = self + retained_total;
        if (filtered_total != 0) {
            destination.times.emplace(window, filtered_total);
        }
    }

    return destination.times.empty() && destination.children.empty() ? NodeResult::Dropped : NodeResult::Retained;
}

}  // namespace

bool isNativeAllocationInstrumentation(std::string_view method_name) noexcept
{
    return matches(method_name, KLinuxMethods) || matches(method_name, KItaniumMethods) ||
           matches(method_name, KSignaturelessMethods);
}

bool isNativeAllocationInstrumentationAddress(std::uint64_t raw_address,
                                              std::span<const NativeInstrumentationRange> ranges) noexcept
{
    return std::ranges::any_of(ranges, [raw_address](const NativeInstrumentationRange &range) {
        return range.begin < range.end && raw_address >= range.begin && raw_address < range.end;
    });
}

bool filterExecutionTree(CallTree &filtered, const CallTree &source, const ResolvedFrameMap &resolved)
{
    const std::vector<NativeInstrumentationRange> instrumentation_ranges = discoverAllocationInstrumentationRanges();
    return filterExecutionTree(filtered, source, resolved, instrumentation_ranges);
}

bool filterExecutionTree(CallTree &filtered, const CallTree &source, const ResolvedFrameMap &resolved,
                         std::span<const NativeInstrumentationRange> instrumentation_ranges)
{
    CallTree candidate;
    if (copyFilteredNode(source.root(), candidate.root(), resolved, instrumentation_ranges) == NodeResult::Malformed) {
        return false;
    }
    filtered = std::move(candidate);
    return true;
}

void filterExecutionTrees(std::vector<ThreadTreeView> &views, std::vector<std::unique_ptr<CallTree>> &owned_trees,
                          const ResolvedFrameMap &resolved)
{
    const std::vector<NativeInstrumentationRange> instrumentation_ranges = discoverAllocationInstrumentationRanges();
    filterExecutionTrees(views, owned_trees, resolved, instrumentation_ranges);
}

void filterExecutionTrees(std::vector<ThreadTreeView> &views, std::vector<std::unique_ptr<CallTree>> &owned_trees,
                          const ResolvedFrameMap &resolved,
                          std::span<const NativeInstrumentationRange> instrumentation_ranges)
{
    for (ThreadTreeView &view : views) {
        if (view.tree == nullptr) {
            continue;
        }
        auto filtered = std::make_unique<CallTree>();
        if (!filterExecutionTree(*filtered, *view.tree, resolved, instrumentation_ranges)) {
            mergeCallTree(*filtered, *view.tree);
        }
        view.tree = filtered.get();
        owned_trees.push_back(std::move(filtered));
    }
}

}  // namespace spark
