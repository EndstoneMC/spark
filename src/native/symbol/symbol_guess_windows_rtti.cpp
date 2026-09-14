#include "native/symbol/symbol_guess_windows_internal.h"

#ifdef _WIN32

#include "native/symbol/dbghelp_manager.h"

// clang-format off
#include <dbghelp.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace spark::symbol_guess::windows {

namespace {

constexpr std::size_t kMaximumTypeDescriptorBytes = 1024;
constexpr std::size_t kMaximumTypeNameBytes = 512;
constexpr std::size_t kMaximumTypeNameAttempts = 4096;
constexpr std::size_t kUnDecorateBufferBytes = 2048;

bool printableAscii(std::string_view value)
{
    return std::ranges::all_of(value, [](unsigned char c) { return c >= 0x20 && c <= 0x7e; });
}

bool isComplexTypeDescriptor(std::string_view raw)
{
    if ((!raw.starts_with(".?AV") && !raw.starts_with(".?AU")) || raw.size() < 6) {
        return false;
    }
    std::string_view encoded = raw.substr(3, raw.size() - 5);
    if (!encoded.empty() && (encoded.front() == 'V' || encoded.front() == 'U')) {
        encoded.remove_prefix(1);
    }
    if (encoded.find('$') != std::string_view::npos && encoded.find('@') == std::string_view::npos) {
        return false;
    }
    return encoded.find('?') != std::string_view::npos || encoded.find('$') != std::string_view::npos;
}

}  // namespace

std::optional<std::string> Engine::Impl::decodeTypeDescriptorName(std::uint32_t rva, TypeNameCache &names)
{
    if (const auto it = names.entries.find(rva); it != names.entries.end()) {
        ++stats.rtti_name_cache_hits;
        return it->second.name;
    }
    if (names.entries.size() >= kMaximumTypeNameAttempts) {
        ++stats.rtti_name_budget_exhausted;
        return std::nullopt;
    }

    ++stats.rtti_name_attempts;
    std::optional<std::string> result;
    std::string raw_encoding;
    std::uint32_t name_rva = 0;
    if (!detail::checkedAdd(rva, 16U, name_rva)) {
        ++stats.rtti_name_failures;
    }
    else if (const Section *section = sectionContaining(name_rva); section == nullptr || section->executable) {
        ++stats.rtti_name_failures;
    }
    else {
        const std::uint32_t available = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(kMaximumTypeDescriptorBytes + 1), section->end - name_rva);
        const char *text = reinterpret_cast<const char *>(image + name_rva);
        std::size_t length = 0;
        bool terminated = false;
        for (; length < available; ++length) {
            const auto c = static_cast<unsigned char>(text[length]);
            if (c == 0) {
                terminated = true;
                break;
            }
            if (c < 0x20 || c > 0x7e) {
                ++stats.rtti_name_failures;
                break;
            }
        }
        if (!terminated) {
            ++stats.rtti_name_length_rejections;
            ++stats.rtti_name_raw_length_rejections;
        }
        else if (length == 0 || length > kMaximumTypeDescriptorBytes) {
            ++stats.rtti_name_length_rejections;
        }
        else {
            const std::string raw(text, length);
            raw_encoding = raw;
            if (!raw.starts_with(".?AV") && !raw.starts_with(".?AU")) {
                ++stats.rtti_name_failures;
            }
            else if (!isComplexTypeDescriptor(raw) && !raw.ends_with("@@")) {
                ++stats.rtti_name_failures;
            }
            else if (!isComplexTypeDescriptor(raw)) {
                std::string plain = detail::classNameFromTypeDescriptor(raw);
                if (plain.empty() || plain.size() > kMaximumTypeNameBytes) {
                    ++stats.rtti_name_length_rejections;
                    ++stats.rtti_name_output_length_rejections;
                }
                else {
                    ++stats.rtti_name_plain;
                    result = std::move(plain);
                }
            }
            else {
                std::string decorated("??_R0");
                decorated.append(raw.substr(1));
                decorated += "@8";
                std::array<char, kUnDecorateBufferBytes> output{};
                DWORD returned = 0;
                {
                    std::scoped_lock lock(::spark::dbgHelpMutex());
                    ++stats.rtti_name_api_calls;
                    returned = UnDecorateSymbolName(decorated.c_str(), output.data(), output.size(), UNDNAME_NAME_ONLY);
                }
                constexpr std::string_view suffix = " `RTTI Type Descriptor'";
                const bool valid_return = returned > 0 && returned < output.size() - 1 && output[returned] == '\0' &&
                                          std::memchr(output.data(), '\0', returned) == nullptr;
                if (!valid_return) {
                    ++stats.rtti_name_failures;
                }
                else {
                    std::string decoded(output.data(), returned);
                    if (!printableAscii(decoded) || !decoded.ends_with(suffix)) {
                        ++stats.rtti_name_failures;
                    }
                    else {
                        decoded.resize(decoded.size() - suffix.size());
                        if (decoded.empty() || decoded.size() > kMaximumTypeNameBytes || decoded == decorated) {
                            ++stats.rtti_name_length_rejections;
                            ++stats.rtti_name_output_length_rejections;
                        }
                        else {
                            ++stats.rtti_name_complex;
                            result = std::move(decoded);
                        }
                    }
                }
            }
        }
    }
    names.entries.emplace(rva, TypeNameCacheEntry{.name = result, .raw = raw_encoding});
    if (result.has_value()) {
        const auto [first, inserted] = names.first_raw.try_emplace(*result, raw_encoding);
        if (!inserted && first->second != raw_encoding && names.ambiguous_names.insert(*result).second) {
            ++stats.rtti_name_collisions;
        }
    }
    stats.rtti_name_cache_entries = names.entries.size();
    return result;
}

std::optional<Engine::Impl::ValidatedCol> Engine::Impl::validateCol(std::uint32_t rva, TypeNameCache &names)
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
    const auto class_name = decodeTypeDescriptorName(col.type_descriptor, names);
    return ValidatedCol{.locator = col,
                        .class_name = class_name,
                        .name_ambiguous = class_name.has_value() && names.ambiguous_names.contains(*class_name)};
}

std::optional<std::uint32_t> Engine::Impl::directThunkTarget(std::uint32_t rva, bool *recognized)
{
    if (recognized != nullptr) {
        *recognized = false;
    }
    const Section *section = sectionContaining(rva);
    if (section == nullptr || !section->executable) {
        return std::nullopt;
    }
    ++stats.thunk_candidates;
    const FunctionRange *source = containing(rva);
    if (source != nullptr && source->begin != rva) {
        ++stats.thunk_interior_destination_rejections;
        return std::nullopt;
    }
    const std::uint32_t source_end = source != nullptr ? source->end : section->end;
    const std::uint32_t available = std::min<std::uint32_t>(source_end - rva, 24);
    if (available < 5) {
        return std::nullopt;
    }
    const std::uint8_t *code = image + rva;
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
    if (recognized != nullptr) {
        *recognized = true;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, code + prefix + 1, sizeof(displacement));
    const std::int64_t target = static_cast<std::int64_t>(rva) + prefix + 5 + displacement;
    if (target < 0 || std::cmp_greater(target, std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
    }
    const auto destination = static_cast<std::uint32_t>(target);
    const FunctionRange *function = containing(destination);
    const FunctionRange *primary = function != nullptr ? containing(function->root) : nullptr;
    if (function == nullptr || function->begin != destination || primary == nullptr ||
        primary->begin != function->root || primary->root != function->root) {
        if (function != nullptr) {
            ++stats.thunk_interior_destination_rejections;
        }
        return std::nullopt;
    }
    ++stats.thunk_resolved;
    return function->root;
}

void Engine::Impl::collectVtables()
{
    TypeNameCache names;
    std::unordered_map<std::uint32_t, std::vector<VtableEvidence>> candidates;
    for (const Section &section : sections) {
        if (section.executable) {
            continue;
        }
        const std::uint32_t available = section.end - section.begin;
        if (available < 16U) {
            continue;
        }
        const std::uint32_t delta = (8U - (section.begin & 7U)) & 7U;
        if (delta > available - 16U) {
            continue;
        }
        for (std::uint32_t offset = delta; offset <= available - 16U; offset += 8U) {
            const std::uint32_t rva = section.begin + offset;
            std::uint64_t col_pointer = 0;
            std::memcpy(&col_pointer, image + rva, sizeof(col_pointer));
            std::uint32_t col_rva = 0;
            if (!toRva(col_pointer, col_rva)) {
                continue;
            }
            const auto validated = validateCol(col_rva, names);
            if (!validated) {
                continue;
            }
            ++stats.vtables;
            if (!validated->class_name) {
                continue;
            }
            const std::string &class_name = *validated->class_name;
            const CompleteObjectLocator &col = validated->locator;
            const std::uint32_t table_offset = offset + 8U;
            bool saw_code = false;
            unsigned external_holes = 0;
            for (std::uint32_t slot = 0; slot < 512; ++slot) {
                const std::uint64_t entry_offset = static_cast<std::uint64_t>(table_offset) + 8ULL * slot;
                if (entry_offset > static_cast<std::uint64_t>(available) - 8U) {
                    break;
                }
                std::uint64_t entry = 0;
                std::memcpy(&entry, image + section.begin + static_cast<std::uint32_t>(entry_offset), sizeof(entry));
                std::uint32_t target = 0;
                if (!toRva(entry, target)) {
                    // Permit one external _purecall-like slot after the table has started.
                    if (entry != 0 && saw_code && external_holes++ == 0) {
                        continue;
                    }
                    break;
                }
                if (validateCol(target, names)) {
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
                    if (function->begin != target) {
                        ++stats.vtable_interior_target_rejections;
                        continue;
                    }
                    const FunctionRange *primary = containing(function->root);
                    if (primary == nullptr || primary->begin != function->root || primary->root != function->root) {
                        continue;
                    }
                    bool recognized_thunk = false;
                    const auto thunk = directThunkTarget(target, &recognized_thunk);
                    if (recognized_thunk) {
                        if (!thunk) {
                            continue;
                        }
                        root = *thunk;
                        via_thunk = true;
                    }
                    else {
                        root = function->root;
                    }
                }
                else {
                    bool recognized_thunk = false;
                    const auto thunk = directThunkTarget(target, &recognized_thunk);
                    if (!recognized_thunk || !thunk) {
                        continue;
                    }
                    root = *thunk;
                    via_thunk = true;
                }
                candidates[root].push_back(
                    {.class_name = class_name, .slot = slot, .secondary = col.offset != 0, .via_thunk = via_thunk});
                ++stats.vtable_candidates;
            }
        }
    }
    for (auto &[root, evidence] : candidates) {
        if (std::ranges::any_of(evidence, [&names](const VtableEvidence &item) {
                return names.ambiguous_names.contains(item.class_name);
            })) {
            ++stats.rtti_name_collision_roots;
            ++stats.vtable_conflicts;
            continue;
        }
        TypedLabel label = ::spark::symbol_guess::windows::chooseVtableLabel(evidence);
        if (label.empty()) {
            ++stats.vtable_conflicts;
            continue;
        }
        vtable_labels.emplace(root, std::move(label));
        ++stats.vtable_labels;
    }
}

}  // namespace spark::symbol_guess::windows

#endif
