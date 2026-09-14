#include "native/symbol/symbol_guess_windows_internal.h"

#ifdef _WIN32

#include <algorithm>
#include <limits>
#include <ranges>

namespace spark::symbol_guess::windows {

bool Engine::Impl::validFunction(const RUNTIME_FUNCTION &function) const
{
    if (function.BeginAddress == 0 || function.EndAddress <= function.BeginAddress) {
        return false;
    }
    const Section *code = sectionContaining(function.BeginAddress, function.EndAddress - function.BeginAddress);
    if (code == nullptr || !code->executable) {
        return false;
    }
    if ((function.UnwindData & RUNTIME_FUNCTION_INDIRECT) != 0) {
        RUNTIME_FUNCTION indirect{};
        return read(function.UnwindData & ~RUNTIME_FUNCTION_INDIRECT, indirect);
    }
    std::uint8_t header[4]{};
    if (!read(function.UnwindData, header)) {
        return false;
    }
    const std::uint8_t version = header[0] & 0x7;
    const std::uint8_t flags = header[0] >> 3;
    return version == 1 && (flags & ~(UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER | UNW_FLAG_CHAININFO)) == 0;
}

std::optional<std::uint32_t> Engine::Impl::chainRoot(const RUNTIME_FUNCTION &start) const
{
    RUNTIME_FUNCTION function = start;
    std::array<RUNTIME_FUNCTION, 32> seen{};
    std::size_t seen_count = 0;

    if (validFunction(function) && (function.UnwindData & RUNTIME_FUNCTION_INDIRECT) == 0) {
        std::uint8_t header[4]{};
        if (read(function.UnwindData, header) && ((header[0] >> 3) & UNW_FLAG_CHAININFO) == 0) {
            return function.BeginAddress;
        }
    }

    for (unsigned depth = 0; depth < 32; ++depth) {
        if (!validFunction(function)) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < seen_count; ++i) {
            if (seen[i].BeginAddress == function.BeginAddress && seen[i].EndAddress == function.EndAddress &&
                seen[i].UnwindData == function.UnwindData) {
                return std::nullopt;
            }
        }
        seen[seen_count++] = function;
        if ((function.UnwindData & RUNTIME_FUNCTION_INDIRECT) != 0) {
            if (!read(function.UnwindData & ~RUNTIME_FUNCTION_INDIRECT, function)) {
                return std::nullopt;
            }
            continue;
        }
        std::uint8_t header[4]{};
        if (!read(function.UnwindData, header)) {
            return std::nullopt;
        }
        const std::uint8_t flags = header[0] >> 3;
        if ((flags & UNW_FLAG_CHAININFO) == 0) {
            return function.BeginAddress;
        }
        if ((flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) != 0) {
            return std::nullopt;
        }
        const std::uint32_t slots = (static_cast<std::uint32_t>(header[2]) + 1U) & ~1U;
        std::uint32_t chained_rva = 0;
        if (!detail::checkedAdd(function.UnwindData, 4U + 2U * slots, chained_rva) || !read(chained_rva, function)) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void Engine::Impl::collectFunctions()
{
    if (exception.VirtualAddress == 0 || exception.Size < sizeof(RUNTIME_FUNCTION) ||
        exception.Size % sizeof(RUNTIME_FUNCTION) != 0 ||
        sectionContaining(exception.VirtualAddress, exception.Size) == nullptr) {
        return;
    }
    const std::uint32_t count = exception.Size / sizeof(RUNTIME_FUNCTION);
    ranges.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        RUNTIME_FUNCTION function{};
        if (!read(exception.VirtualAddress + i * sizeof(RUNTIME_FUNCTION), function)) {
            ++stats.rejected_ranges;
            continue;
        }
        const std::optional<std::uint32_t> root = chainRoot(function);
        if (!root) {
            ++stats.rejected_ranges;
            continue;
        }
        ranges.push_back({.begin = function.BeginAddress, .end = function.EndAddress, .root = *root});
        stats.chained_ranges += *root != function.BeginAddress ? 1 : 0;
    }
    std::ranges::sort(ranges, [](const FunctionRange &a, const FunctionRange &b) {
        if (a.begin != b.begin) {
            return a.begin < b.begin;
        }
        if (a.end != b.end) {
            return a.end < b.end;
        }
        return a.root < b.root;
    });
    const auto duplicate = std::ranges::unique(ranges);
    ranges.erase(duplicate.begin(), duplicate.end());

    std::vector<bool> ambiguous(ranges.size(), false);
    std::size_t cluster_begin = 0;
    while (cluster_begin < ranges.size()) {
        std::size_t cluster_end = cluster_begin + 1;
        std::uint32_t maximum_end = ranges[cluster_begin].end;
        while (cluster_end < ranges.size() && ranges[cluster_end].begin < maximum_end) {
            maximum_end = std::max(maximum_end, ranges[cluster_end].end);
            ++cluster_end;
        }
        if (cluster_end - cluster_begin > 1) {
            for (std::size_t i = cluster_begin; i < cluster_end; ++i) {
                ambiguous[i] = true;
                ++stats.overlap_ranges;
            }
        }
        cluster_begin = cluster_end;
    }
    std::vector<FunctionRange> safe;
    safe.reserve(ranges.size());
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (!ambiguous[i]) {
            safe.push_back(ranges[i]);
        }
    }
    ranges = std::move(safe);
    stats.function_ranges = ranges.size();
    for (const FunctionRange &range : ranges) {
        if (range.root != range.begin) {
            chained_fragment_starts[range.root].push_back(range.begin);
        }
    }
}

const FunctionRange *Engine::Impl::containing(std::uint64_t rva) const
{
    if (rva > std::numeric_limits<std::uint32_t>::max()) {
        return nullptr;
    }
    // NOLINTNEXTLINE(modernize-use-ranges)
    auto it = std::upper_bound(ranges.begin(), ranges.end(), static_cast<std::uint32_t>(rva),
                               [](std::uint32_t value, const FunctionRange &range) { return value < range.begin; });
    if (it == ranges.begin()) {
        return nullptr;
    }
    --it;
    return rva < it->end ? &*it : nullptr;
}

}  // namespace spark::symbol_guess::windows

#endif
