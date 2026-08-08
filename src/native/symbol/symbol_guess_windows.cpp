#include "native/symbol/symbol_guess_windows.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
// windows.h must precede psapi.h; keep clang-format from sorting this pair.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <distorm.h>
#include <mnemonics.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace spark::symbol_guess::windows {

namespace {

using Clock = std::chrono::steady_clock;

struct Section {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    bool executable = false;
};

struct CompleteObjectLocator {
    std::uint32_t signature;
    std::uint32_t offset;
    std::uint32_t cd_offset;
    std::uint32_t type_descriptor;
    std::uint32_t class_descriptor;
    std::uint32_t self;
};

struct ClassHierarchyDescriptor {
    std::uint32_t signature;
    std::uint32_t attributes;
    std::uint32_t base_count;
    std::uint32_t base_array;
};

struct StringCandidate {
    std::uint32_t target = 0;
    std::string value;
    int score = 0;
};

bool checkedAdd(std::uint32_t a, std::uint32_t b, std::uint32_t &out)
{
    if (b > std::numeric_limits<std::uint32_t>::max() - a) {
        return false;
    }
    out = a + b;
    return true;
}

std::string classNameFromTypeDescriptor(std::string_view mangled)
{
    if (!mangled.starts_with(".?A")) {
        return {};
    }
    std::string_view encoded = mangled.substr(3);
    if (!encoded.empty() && (encoded.front() == 'V' || encoded.front() == 'U')) {
        encoded.remove_prefix(1);
    }
    const std::size_t end = encoded.find("@@");
    if (end == std::string_view::npos || end == 0) {
        return {};
    }
    encoded = encoded.substr(0, end);
    // Anonymous scopes and templates need a full MSVC demangler. A raw encoded
    // type is worse than no class hint, so keep this conservative.
    if (encoded.find('?') != std::string_view::npos || encoded.find('$') != std::string_view::npos) {
        return {};
    }
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= encoded.size()) {
        const std::size_t at = encoded.find('@', start);
        if (at == std::string_view::npos) {
            parts.push_back(encoded.substr(start));
            break;
        }
        parts.push_back(encoded.substr(start, at - start));
        start = at + 1;
    }
    if (std::ranges::any_of(parts, [](std::string_view part) { return part.empty(); })) {
        return {};
    }
    std::string out;
    for (auto &part : std::views::reverse(parts)) {
        if (!out.empty()) {
            out += "::";
        }
        out += part;
    }
    if (out.size() > 80) {
        return {};
    }
    return out;
}

}  // namespace

TypedLabel chooseVtableLabel(std::vector<VtableEvidence> evidence)
{
    return ::spark::symbol_guess::chooseVtableLabel(std::move(evidence), nullptr);
}

int scoreStringHint(std::string_view value)
{
    return ::spark::symbol_guess::scoreStringHint(value);
}

TypedLabel formatStringHint(std::string_view value)
{
    return ::spark::symbol_guess::formatStringHint(value, scoreStringHint(value));
}

struct Engine::Impl {
    const std::uint8_t *image = nullptr;
    std::size_t mapped_size = 0;
    std::uint64_t load_address = 0;
    std::uint32_t image_size = 0;
    IMAGE_DATA_DIRECTORY exception{};
    std::vector<Section> sections;
    std::vector<FunctionRange> ranges;
    // Only chained roots need an auxiliary list. Ordinary functions are found
    // directly in the sorted range index, avoiding one hash allocation per one
    // of the hundreds of thousands of .pdata entries in BDS.
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> chained_fragment_starts;
    std::unordered_map<std::uint32_t, TypedLabel> vtable_labels;
    mutable std::mutex mutex;
    BuildStats stats;

    template <typename T>
    bool readRaw(std::size_t offset, T &out) const
    {
        if (offset > mapped_size || sizeof(T) > mapped_size - offset) {
            return false;
        }
        std::memcpy(&out, image + offset, sizeof(T));
        return true;
    }

    const Section *sectionContaining(std::uint32_t rva, std::uint32_t length = 1) const
    {
        auto it = std::upper_bound(sections.begin(), sections.end(), rva,  // NOLINT(modernize-use-ranges)
                                   [](std::uint32_t value, const Section &section) { return value < section.begin; });
        if (it == sections.begin()) {
            return nullptr;
        }
        --it;
        return rva >= it->begin && rva < it->end && length <= it->end - rva ? &*it : nullptr;
    }

    template <typename T>
    bool read(std::uint32_t rva, T &out) const
    {
        if (sectionContaining(rva, static_cast<std::uint32_t>(sizeof(T))) == nullptr) {
            return false;
        }
        std::memcpy(&out, image + rva, sizeof(T));
        return true;
    }

    bool toRva(std::uint64_t pointer, std::uint32_t &rva) const
    {
        if (pointer < load_address || pointer - load_address >= image_size ||
            pointer - load_address > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        rva = static_cast<std::uint32_t>(pointer - load_address);
        return sectionContaining(rva) != nullptr;
    }

    std::string readCString(std::uint32_t rva, std::uint32_t maximum) const
    {
        const Section *section = sectionContaining(rva);
        if (section == nullptr || section->executable) {
            return {};
        }
        const std::uint32_t limit = std::min(maximum, section->end - rva);
        const char *text = reinterpret_cast<const char *>(image + rva);
        for (std::uint32_t i = 0; i < limit; ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (c == 0) {
                return {text, i};
            }
            if (c < 0x20 || c > 0x7e) {
                return {};
            }
        }
        return {};
    }

    bool parseHeaders()
    {
        if (image == nullptr || mapped_size < sizeof(IMAGE_DOS_HEADER)) {
            return false;
        }
        IMAGE_DOS_HEADER dos{};
        if (!readRaw(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
            return false;
        }
        const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
        IMAGE_NT_HEADERS64 nt{};
        if (!readRaw(nt_offset, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt.OptionalHeader.SizeOfImage == 0 || nt.OptionalHeader.SizeOfImage > mapped_size ||
            nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
            return false;
        }
        image_size = nt.OptionalHeader.SizeOfImage;
        stats.image_bytes = image_size;
        exception = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];

        const std::size_t section_offset =
            nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
        if (nt.FileHeader.NumberOfSections == 0 || section_offset > mapped_size ||
            static_cast<std::size_t>(nt.FileHeader.NumberOfSections) >
                (mapped_size - section_offset) / sizeof(IMAGE_SECTION_HEADER)) {
            return false;
        }
        for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER header{};
            if (!readRaw(section_offset + i * sizeof(header), header)) {
                return false;
            }
            if ((header.Characteristics & IMAGE_SCN_MEM_READ) == 0 || header.Misc.VirtualSize == 0 ||
                (header.Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) != 0) {
                continue;
            }
            std::uint32_t end = 0;
            if (!checkedAdd(header.VirtualAddress, header.Misc.VirtualSize, end) || end > image_size ||
                header.VirtualAddress >= end) {
                continue;
            }
            sections.push_back({.begin = header.VirtualAddress,
                                .end = end,
                                .executable = (header.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
        }
        std::ranges::sort(sections, [](const Section &a, const Section &b) {
            return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
        });
        // Ambiguous overlapping section permissions make every later validation
        // suspect. Reject the image instead of guessing through it.
        for (std::size_t i = 1; i < sections.size(); ++i) {
            if (sections[i].begin < sections[i - 1].end) {
                sections.clear();
                return false;
            }
        }
        return !sections.empty();
    }

    bool validFunction(const RUNTIME_FUNCTION &function) const
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

    std::optional<std::uint32_t> chainRoot(const RUNTIME_FUNCTION &start) const
    {
        RUNTIME_FUNCTION function = start;
        std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> seen;
        for (unsigned depth = 0; depth < 32; ++depth) {
            if (!validFunction(function) ||
                !seen.emplace(function.BeginAddress, function.EndAddress, function.UnwindData).second) {
                return std::nullopt;
            }
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
            if (!checkedAdd(function.UnwindData, 4U + 2U * slots, chained_rva) || !read(chained_rva, function)) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    void collectFunctions()
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

    const FunctionRange *containing(std::uint64_t rva) const
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

    std::optional<std::pair<std::string, CompleteObjectLocator>> validateCol(std::uint32_t rva) const
    {
        CompleteObjectLocator col{};
        if (!read(rva, col) || col.signature != 1 || col.self != rva) {
            return std::nullopt;
        }
        ClassHierarchyDescriptor hierarchy{};
        if (!read(col.class_descriptor, hierarchy) || hierarchy.signature != 0 || hierarchy.base_count == 0 ||
            hierarchy.base_count > 1024) {
            return std::nullopt;
        }
        const Section *base_array = sectionContaining(hierarchy.base_array, hierarchy.base_count * 4U);
        if (base_array == nullptr || base_array->executable) {
            return std::nullopt;
        }
        std::uint32_t first_base = 0;
        std::uint32_t first_type = 0;
        std::uint32_t contained_bases = 0;
        if (!read(hierarchy.base_array, first_base) || !read(first_base, first_type) ||
            !read(first_base + 4, contained_bases) || first_type != col.type_descriptor ||
            contained_bases > hierarchy.base_count) {
            return std::nullopt;
        }
        const std::string mangled = readCString(col.type_descriptor + 16, 256);
        std::string class_name = classNameFromTypeDescriptor(mangled);
        if (class_name.empty()) {
            return std::nullopt;
        }
        return std::pair<std::string, CompleteObjectLocator>{std::move(class_name), col};
    }

    std::optional<std::uint32_t> directThunkTarget(std::uint32_t rva)
    {
        const Section *section = sectionContaining(rva);
        if (section == nullptr || !section->executable) {
            return std::nullopt;
        }
        ++stats.thunk_candidates;
        std::uint32_t cursor = rva;
        const std::uint32_t available = std::min<std::uint32_t>(section->end - cursor, 24);
        if (available < 5) {
            return std::nullopt;
        }
        const std::uint8_t *code = image + cursor;
        std::uint32_t prefix = 0;
        if (available >= 9 && code[0] == 0x48 &&
            ((code[1] == 0x83 && (code[2] == 0xe9 || code[2] == 0xc1)) || (code[1] == 0x8d && code[2] == 0x49))) {
            prefix = 4;
        }
        else if (available >= 12 && code[0] == 0x48 &&
                 ((code[1] == 0x81 && (code[2] == 0xe9 || code[2] == 0xc1)) || (code[1] == 0x8d && code[2] == 0x89))) {
            prefix = 7;
        }
        if (available < prefix + 5 || code[prefix] != 0xe9) {
            return std::nullopt;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, code + prefix + 1, sizeof(displacement));
        const std::int64_t target = static_cast<std::int64_t>(rva) + prefix + 5 + displacement;
        if (target < 0 || std::cmp_greater(target, std::numeric_limits<std::uint32_t>::max())) {
            return std::nullopt;
        }
        const FunctionRange *function = containing(static_cast<std::uint32_t>(target));
        if (function == nullptr) {
            return std::nullopt;
        }
        ++stats.thunk_resolved;
        return function->root;
    }

    void collectVtables()
    {
        std::unordered_map<std::uint32_t, std::vector<VtableEvidence>> candidates;
        for (const Section &section : sections) {
            if (section.executable) {
                continue;
            }
            const std::uint32_t start = (section.begin + 7U) & ~7U;
            for (std::uint32_t rva = start; rva <= section.end - 16U; rva += 8) {
                std::uint64_t col_pointer = 0;
                std::memcpy(&col_pointer, image + rva, sizeof(col_pointer));
                std::uint32_t col_rva = 0;
                if (!toRva(col_pointer, col_rva)) {
                    continue;
                }
                const auto validated = validateCol(col_rva);
                if (!validated) {
                    continue;
                }
                const auto &[class_name, col] = *validated;
                ++stats.vtables;
                const std::uint32_t table = rva + 8;
                bool saw_code = false;
                unsigned external_holes = 0;
                for (std::uint32_t slot = 0; slot < 512 && table <= section.end - 8U * (slot + 1U); ++slot) {
                    std::uint64_t entry = 0;
                    std::memcpy(&entry, image + table + 8U * slot, sizeof(entry));
                    std::uint32_t target = 0;
                    if (!toRva(entry, target)) {
                        // Permit one external _purecall-like slot after the table
                        // has started, but never walk through zero/padding.
                        if (entry != 0 && saw_code && external_holes++ == 0) {
                            continue;
                        }
                        break;
                    }
                    if (validateCol(target)) {
                        break;
                    }
                    const Section *target_section = sectionContaining(target);
                    if (target_section == nullptr || !target_section->executable) {
                        break;
                    }
                    saw_code = true;
                    external_holes = 0;
                    const FunctionRange *function = containing(target);
                    bool via_thunk = false;
                    std::uint32_t root = 0;
                    if (function != nullptr) {
                        root = function->root;
                    }
                    else if (const auto thunk = directThunkTarget(target)) {
                        root = *thunk;
                        via_thunk = true;
                    }
                    else {
                        continue;
                    }
                    candidates[root].push_back(
                        {.class_name = class_name, .slot = slot, .secondary = col.offset != 0, .via_thunk = via_thunk});
                    ++stats.vtable_candidates;
                }
            }
        }
        for (auto &[root, evidence] : candidates) {
            TypedLabel label = ::spark::symbol_guess::windows::chooseVtableLabel(evidence);
            if (label.empty()) {
                ++stats.vtable_conflicts;
                continue;
            }
            vtable_labels.emplace(root, std::move(label));
            ++stats.vtable_labels;
        }
    }

    const FunctionRange *fragmentContaining(std::uint32_t root, std::uint32_t rva) const
    {
        const FunctionRange *range = containing(rva);
        return range != nullptr && range->root == root ? range : nullptr;
    }

    std::vector<StringCandidate> decodeStrings(std::uint32_t root, BuildStats &batch) const
    {
        const FunctionRange *root_range = containing(root);
        if (root_range == nullptr || root_range->begin != root || root_range->root != root) {
            return {};
        }
        std::vector<std::uint32_t> work{root};
        if (const auto fragments = chained_fragment_starts.find(root); fragments != chained_fragment_starts.end()) {
            work.insert(work.end(), fragments->second.begin(), fragments->second.end());
        }
        std::unordered_set<std::uint32_t> visited;
        std::map<std::uint32_t, StringCandidate> candidates;

        while (!work.empty()) {
            std::uint32_t cursor = work.back();
            work.pop_back();
            while (const FunctionRange *fragment = fragmentContaining(root, cursor)) {
                if (!visited.insert(cursor).second) {
                    break;
                }
                _CodeInfo info{};
                info.codeOffset = cursor;
                info.code = image + cursor;
                info.codeLen = static_cast<int>(fragment->end - cursor);
                info.dt = Decode64Bits;
                info.features = DF_STOP_ON_FLOW_CONTROL | DF_STOP_ON_UNDECODEABLE;
                _DInst instructions[64]{};
                unsigned used = 0;
                const _DecodeResult result = distorm_decompose64(&info, instructions, 64, &used);
                if ((result == DECRES_INPUTERR || result == DECRES_NONE) || used == 0) {
                    break;
                }
                bool stop = false;
                for (unsigned i = 0; i < used; ++i) {
                    const _DInst &instruction = instructions[i];
                    if (instruction.flags == FLAG_NOT_DECODABLE || instruction.size == 0 ||
                        instruction.addr > std::numeric_limits<std::uint32_t>::max()) {
                        stop = true;
                        break;
                    }
                    const auto address = static_cast<std::uint32_t>(instruction.addr);
                    if (address != cursor && !visited.insert(address).second) {
                        stop = true;
                        break;
                    }
                    ++batch.decoded_instructions;
                    if (instruction.opcode == I_LEA && (instruction.flags & FLAG_RIP_RELATIVE) != 0) {
                        const std::uint64_t target_wide = INSTRUCTION_GET_RIP_TARGET(&instruction);
                        if (target_wide <= std::numeric_limits<std::uint32_t>::max()) {
                            const auto target = static_cast<std::uint32_t>(target_wide);
                            std::string value = readCString(target, 180);
                            const int score = scoreStringHint(value);
                            if (score >= ::spark::symbol_guess::kMinimumStringHintScore) {
                                candidates.try_emplace(
                                    target,
                                    StringCandidate{.target = target, .value = std::move(value), .score = score});
                            }
                        }
                    }

                    const unsigned flow = META_GET_FC(instruction.meta);
                    if (flow == FC_CND_BRANCH || flow == FC_UNC_BRANCH) {
                        for (const _Operand &operand : instruction.ops) {
                            if (operand.type == O_PC) {
                                const std::uint64_t target_wide = INSTRUCTION_GET_TARGET(&instruction);
                                if (target_wide <= std::numeric_limits<std::uint32_t>::max() &&
                                    fragmentContaining(root, static_cast<std::uint32_t>(target_wide)) != nullptr) {
                                    work.push_back(static_cast<std::uint32_t>(target_wide));
                                }
                                break;
                            }
                        }
                    }
                    cursor = address + instruction.size;
                    if (flow == FC_RET || flow == FC_SYS || flow == FC_UNC_BRANCH || flow == FC_INT || flow == FC_HLT) {
                        stop = true;
                    }
                }
                if (stop || fragmentContaining(root, cursor) == nullptr) {
                    break;
                }
            }
        }

        std::vector<StringCandidate> out;
        out.reserve(candidates.size());
        for (auto &[target, candidate] : candidates) {
            out.push_back(std::move(candidate));
        }
        std::ranges::sort(out, [](const StringCandidate &a, const StringCandidate &b) {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            if (a.value != b.value) {
                return a.value < b.value;
            }
            return a.target < b.target;
        });
        batch.string_candidates += out.size();
        return out;
    }

    void scanCandidateReferences(const std::unordered_set<std::uint32_t> &targets,
                                 std::unordered_map<std::uint32_t, std::set<std::uint32_t>> &references) const
    {
        for (const Section &section : sections) {
            if (!section.executable || section.end - section.begin < 7) {
                continue;
            }
            for (std::uint32_t rva = section.begin; rva <= section.end - 7; ++rva) {
                const std::uint8_t *code = image + rva;
                if (code[0] < 0x48 || code[0] > 0x4f || code[1] != 0x8d || (code[2] & 0xc7) != 0x05) {
                    continue;
                }
                std::int32_t displacement = 0;
                std::memcpy(&displacement, code + 3, sizeof(displacement));
                const std::int64_t target_wide = static_cast<std::int64_t>(rva) + 7 + displacement;
                if (target_wide < 0 || std::cmp_greater(target_wide, std::numeric_limits<std::uint32_t>::max())) {
                    continue;
                }
                const auto target = static_cast<std::uint32_t>(target_wide);
                if (!targets.contains(target)) {
                    continue;
                }
                if (const FunctionRange *function = containing(rva)) {
                    references[target].insert(function->root);
                }
            }
        }
    }

    void updateApproximateBytes()
    {
        std::size_t bytes = ranges.capacity() * sizeof(FunctionRange) + sections.capacity() * sizeof(Section);
        for (const auto &[root, fragments] : chained_fragment_starts) {
            bytes += sizeof(root) + fragments.capacity() * sizeof(std::uint32_t) + 32;
        }
        for (const auto &[root, label] : vtable_labels) {
            bytes += sizeof(root) + label.label.capacity() + 48;
        }
        stats.approximate_bytes = bytes;
    }

    void initialize()
    {
        const auto start = Clock::now();
        if (!parseHeaders()) {
            return;
        }
        collectFunctions();
        if (ranges.empty()) {
            return;
        }
        collectVtables();
        updateApproximateBytes();
        stats.initialized = true;
        stats.build_microseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    }

    std::unordered_map<std::uint64_t, TypedLabel> guess(std::span<const std::uint64_t> rvas)
    {
        std::scoped_lock lock(mutex);
        const auto start = Clock::now();
        BuildStats batch = stats;
        batch.batch_microseconds = 0;
        batch.sampled_functions = 0;
        batch.decoded_instructions = 0;
        batch.string_candidates = 0;
        batch.shared_strings = 0;
        batch.string_labels = 0;

        std::unordered_map<std::uint64_t, TypedLabel> out;
        out.reserve(rvas.size());
        std::map<std::uint32_t, std::vector<std::uint64_t>> root_inputs;
        for (std::uint64_t rva : rvas) {
            if (const FunctionRange *function = containing(rva)) {
                root_inputs[function->root].push_back(rva);
            }
        }
        batch.sampled_functions = root_inputs.size();

        std::map<std::uint32_t, std::vector<StringCandidate>> string_candidates;
        std::unordered_set<std::uint32_t> candidate_targets;
        for (const auto &[root, inputs] : root_inputs) {
            if (const auto label = vtable_labels.find(root); label != vtable_labels.end()) {
                for (std::uint64_t rva : inputs) {
                    out.emplace(rva, label->second);
                }
                continue;
            }
            std::vector<StringCandidate> candidates = decodeStrings(root, batch);
            for (const StringCandidate &candidate : candidates) {
                candidate_targets.insert(candidate.target);
            }
            string_candidates.emplace(root, std::move(candidates));
        }

        std::unordered_map<std::uint32_t, std::set<std::uint32_t>> references;
        if (!candidate_targets.empty()) {
            scanCandidateReferences(candidate_targets, references);
        }
        for (const auto &[root, candidates] : string_candidates) {
            TypedLabel label;
            for (const StringCandidate &candidate : candidates) {
                const auto refs = references.find(candidate.target);
                if (refs != references.end() && refs->second.size() == 1 && *refs->second.begin() == root) {
                    label = ::spark::symbol_guess::formatStringHint(candidate.value, candidate.score);
                    break;
                }
                ++batch.shared_strings;
            }
            if (label.empty()) {
                continue;
            }
            ++batch.string_labels;
            for (std::uint64_t rva : root_inputs.at(root)) {
                out.emplace(rva, label);
            }
        }
        batch.batch_microseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
        stats = batch;
        return out;
    }
};

Engine::Engine(const std::uint8_t *image, std::size_t mapped_size, std::uint64_t load_address)
    : impl_(std::make_unique<Impl>())
{
    impl_->image = image;
    impl_->mapped_size = mapped_size;
    impl_->load_address = load_address;
    impl_->initialize();
}

Engine::~Engine() = default;
Engine::Engine(Engine &&) noexcept = default;
Engine &Engine::operator=(Engine &&) noexcept = default;

bool Engine::valid() const
{
    return impl_ != nullptr && impl_->stats.initialized;
}

const FunctionRange *Engine::functionContaining(std::uint64_t rva) const
{
    return impl_ != nullptr ? impl_->containing(rva) : nullptr;
}

std::unordered_map<std::uint64_t, TypedLabel> Engine::guess(std::span<const std::uint64_t> rvas)
{
    return impl_ != nullptr ? impl_->guess(rvas) : std::unordered_map<std::uint64_t, TypedLabel>{};
}

BuildStats Engine::stats() const
{
    if (impl_ == nullptr) {
        return {};
    }
    std::scoped_lock lock(impl_->mutex);
    return impl_->stats;
}

namespace {

struct CurrentEngineState {
    std::mutex mutex;
    std::unique_ptr<Engine> engine;
};

CurrentEngineState &currentEngineState()
{
    static CurrentEngineState state;
    return state;
}

Engine &currentEngine()
{
    CurrentEngineState &state = currentEngineState();
    std::scoped_lock lock(state.mutex);
    if (state.engine != nullptr) {
        return *state.engine;
    }
    state.engine = std::make_unique<Engine>([] {
        HMODULE module = GetModuleHandleW(nullptr);
        MODULEINFO info{};
        if (module == nullptr || GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) == FALSE) {
            return Engine(nullptr, 0, 0);
        }
        return Engine(static_cast<const std::uint8_t *>(info.lpBaseOfDll), info.SizeOfImage,
                      reinterpret_cast<std::uint64_t>(info.lpBaseOfDll));
    }());
    return *state.engine;
}

}  // namespace

std::unordered_map<std::uint64_t, TypedLabel> guessCurrentModuleSymbols(std::span<const std::uint64_t> rvas)
{
    if (rvas.empty()) {
        return {};
    }
    return currentEngine().guess(rvas);
}

BuildStats currentModuleStats()
{
    CurrentEngineState &state = currentEngineState();
    std::scoped_lock lock(state.mutex);
    return state.engine != nullptr ? state.engine->stats() : BuildStats{};
}

}  // namespace spark::symbol_guess::windows

#endif
