#include "sampler/symbol_guess.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spark {

namespace {

struct Section {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    bool executable = false;
};

// One .pdata entry. Fragments of a chained function all share the root begin
// address, which is the key used for labels.
struct FunctionRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::uint32_t root = 0;
};

struct GuessTable {
    std::vector<FunctionRange> ranges;
    std::unordered_map<std::uint32_t, std::string> labels;
};

// x64 MSVC RTTI complete object locator, stored one pointer before each vftable.
struct CompleteObjectLocator {
    std::uint32_t signature;
    std::uint32_t offset;
    std::uint32_t cd_offset;
    std::uint32_t type_descriptor;
    std::uint32_t class_descriptor;
    std::uint32_t self;
};

// Bounds-checked read-only view of the current process's main executable image.
class ImageView {
public:
    bool init()
    {
        base_ = reinterpret_cast<const std::uint8_t *>(GetModuleHandleW(nullptr));
        if (base_ == nullptr) {
            return false;
        }
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base_);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base_ + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return false;
        }
        image_size_ = nt->OptionalHeader.SizeOfImage;
        const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const IMAGE_SECTION_HEADER &s = section[i];
            if ((s.Characteristics & IMAGE_SCN_MEM_READ) == 0 || s.Misc.VirtualSize == 0 ||
                (s.Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) != 0) {
                continue;
            }
            sections_.push_back({s.VirtualAddress, s.VirtualAddress + s.Misc.VirtualSize,
                                 (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
        }
        exception_ = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        return !sections_.empty();
    }

    const std::uint8_t *base() const
    {
        return base_;
    }

    const std::vector<Section> &sections() const
    {
        return sections_;
    }

    const IMAGE_DATA_DIRECTORY &exceptionDirectory() const
    {
        return exception_;
    }

    const Section *sectionContaining(std::uint32_t rva, std::uint32_t length) const
    {
        for (const Section &s : sections_) {
            if (rva >= s.begin && rva < s.end && length <= s.end - rva) {
                return &s;
            }
        }
        return nullptr;
    }

    // True when `pointer` is an absolute virtual address inside this image.
    bool toRva(std::uint64_t pointer, std::uint32_t &rva) const
    {
        const auto begin = reinterpret_cast<std::uint64_t>(base_);
        if (pointer < begin || pointer - begin >= image_size_) {
            return false;
        }
        rva = static_cast<std::uint32_t>(pointer - begin);
        return true;
    }

private:
    const std::uint8_t *base_ = nullptr;
    std::uint64_t image_size_ = 0;
    std::vector<Section> sections_;
    IMAGE_DATA_DIRECTORY exception_{};
};

template <typename T>
bool readAt(const ImageView &img, std::uint32_t rva, T &out)
{
    const Section *section = img.sectionContaining(rva, sizeof(T));
    if (section == nullptr) {
        return false;
    }
    std::memcpy(&out, img.base() + rva, sizeof(T));
    return true;
}

// Reads a NUL-terminated printable-ASCII string of at most `maximum` bytes.
// Returns empty when unterminated, unprintable, or out of bounds.
std::string readCString(const ImageView &img, std::uint32_t rva, std::uint32_t maximum)
{
    const Section *section = img.sectionContaining(rva, 1);
    if (section == nullptr) {
        return {};
    }
    const std::uint32_t limit = std::min(maximum, section->end - rva);
    const char *p = reinterpret_cast<const char *>(img.base() + rva);
    for (std::uint32_t i = 0; i < limit; ++i) {
        const auto c = static_cast<unsigned char>(p[i]);
        if (c == '\0') {
            return std::string(p, i);
        }
        if (c < 0x20 || c > 0x7e) {
            return {};
        }
    }
    return {};
}

// Follows chained/indirect unwind info so that every .pdata fragment of a large
// function resolves to the RUNTIME_FUNCTION that actually begins it.
std::uint32_t chainRootBegin(const ImageView &img, RUNTIME_FUNCTION rf)
{
    for (int depth = 0; depth < 32; ++depth) {
        if ((rf.UnwindData & 1) != 0) {
            RUNTIME_FUNCTION next{};
            if (!readAt(img, rf.UnwindData & ~1u, next)) {
                break;
            }
            rf = next;
            continue;
        }
        std::uint8_t header[4] = {};
        if (!readAt(img, rf.UnwindData, header) || ((header[0] >> 3) & UNW_FLAG_CHAININFO) == 0) {
            break;
        }
        RUNTIME_FUNCTION next{};
        if (!readAt(img, rf.UnwindData + 4 + 2u * ((header[2] + 1u) & ~1u), next)) {
            break;
        }
        rf = next;
    }
    return rf.BeginAddress;
}

void collectFunctions(const ImageView &img, GuessTable &table)
{
    const IMAGE_DATA_DIRECTORY &dir = img.exceptionDirectory();
    const std::uint32_t count = dir.Size / static_cast<std::uint32_t>(sizeof(RUNTIME_FUNCTION));
    table.ranges.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        RUNTIME_FUNCTION rf{};
        if (!readAt(img, dir.VirtualAddress + i * static_cast<std::uint32_t>(sizeof(RUNTIME_FUNCTION)), rf)) {
            break;
        }
        if (rf.BeginAddress == 0 || rf.EndAddress <= rf.BeginAddress) {
            continue;
        }
        table.ranges.push_back({rf.BeginAddress, rf.EndAddress, chainRootBegin(img, rf)});
    }
    std::sort(table.ranges.begin(), table.ranges.end(),
              [](const FunctionRange &a, const FunctionRange &b) { return a.begin < b.begin; });
}

const FunctionRange *functionContaining(const GuessTable &table, std::uint64_t rva)
{
    auto it = std::upper_bound(table.ranges.begin(), table.ranges.end(), rva,
                               [](std::uint64_t value, const FunctionRange &r) { return value < r.begin; });
    if (it == table.ranges.begin()) {
        return nullptr;
    }
    --it;
    return rva < it->end ? &*it : nullptr;
}

// ".?AVServerLevel@@" -> "ServerLevel"; ".?AVChunkSource@detail@@" ->
// "detail::ChunkSource". Template and other complex encodings are returned raw.
std::string classNameFromTypeDescriptor(std::string_view mangled)
{
    if (mangled.rfind(".?A", 0) != 0) {
        return {};
    }
    std::string_view s = mangled.substr(3);
    if (!s.empty() && (s[0] == 'V' || s[0] == 'U')) {
        s.remove_prefix(1);
    }
    const auto end = s.find("@@");
    if (end == std::string_view::npos || end == 0) {
        return {};
    }
    s = s.substr(0, end);
    if (s.find('?') != std::string_view::npos) {
        return std::string(s);
    }
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t at = s.find('@', start);
        if (at == std::string_view::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, at - start));
        start = at + 1;
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) {
            out += "::";
        }
        out += *it;
    }
    return out;
}

// Returns the class name when `col_rva` holds a valid complete object locator.
std::string validateCol(const ImageView &img, std::uint32_t col_rva)
{
    CompleteObjectLocator col{};
    if (!readAt(img, col_rva, col) || col.signature != 1 || col.self != col_rva) {
        return {};
    }
    const std::string mangled = readCString(img, col.type_descriptor + 16, 256);
    if (mangled.empty()) {
        return {};
    }
    std::string name = classNameFromTypeDescriptor(mangled);
    if (name.size() > 64) {
        name.resize(61);
        name += "...";
    }
    return name;
}

// Scans data sections for vftables (a pointer to a valid complete object
// locator followed by pointers into executable code) and labels each slot's
// target function "Class::vfn[i]". Writable data is included because linkers
// sometimes place RTTI there; the self-referential locator check keeps false
// positives out.
void collectVtableLabels(const ImageView &img, GuessTable &table)
{
    for (const Section &section : img.sections()) {
        if (section.executable) {
            continue;
        }
        for (std::uint32_t rva = (section.begin + 7u) & ~7u; rva + 16 <= section.end; rva += 8) {
            std::uint64_t col_pointer = 0;
            std::memcpy(&col_pointer, img.base() + rva, 8);
            std::uint32_t col_rva = 0;
            if (!img.toRva(col_pointer, col_rva)) {
                continue;
            }
            const std::string class_name = validateCol(img, col_rva);
            if (class_name.empty()) {
                continue;
            }
            const std::uint32_t vtable = rva + 8;
            for (std::uint32_t slot = 0; vtable + 8u * (slot + 1) <= section.end; ++slot) {
                std::uint64_t entry = 0;
                std::memcpy(&entry, img.base() + vtable + 8u * slot, 8);
                std::uint32_t target = 0;
                if (!img.toRva(entry, target)) {
                    break;
                }
                const Section *target_section = img.sectionContaining(target, 1);
                if (target_section == nullptr || !target_section->executable) {
                    break;
                }
                const FunctionRange *fn = functionContaining(table, target);
                if (fn == nullptr) {
                    continue;
                }
                table.labels.try_emplace(fn->root, class_name + "::vfn[" + std::to_string(slot) + "]");
            }
        }
    }
}

struct StringReference {
    std::uint32_t function = 0;
    std::uint32_t length = 0;
    bool ambiguous = false;
};

// Scans executable sections for RIP-relative LEA instructions whose target is a
// printable NUL-terminated literal. A string referenced by exactly one function
// becomes that function's label when RTTI produced nothing better.
void collectStringLabels(const ImageView &img, GuessTable &table)
{
    std::unordered_map<std::uint32_t, StringReference> references;
    const std::uint8_t *bytes = img.base();
    for (const Section &section : img.sections()) {
        if (!section.executable) {
            continue;
        }
        for (std::uint32_t rva = section.begin; rva + 7 <= section.end; ++rva) {
            if (bytes[rva] < 0x48 || bytes[rva] > 0x4f || bytes[rva + 1] != 0x8d ||
                (bytes[rva + 2] & 0xc7) != 0x05) {
                continue;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement, bytes + rva + 3, 4);
            const std::int64_t wide_target = static_cast<std::int64_t>(rva) + 7 + displacement;
            if (wide_target < 0 || wide_target > 0xffffffffLL) {
                continue;
            }
            const auto target = static_cast<std::uint32_t>(wide_target);
            const Section *target_section = img.sectionContaining(target, 1);
            if (target_section == nullptr || target_section->executable) {
                continue;
            }
            const FunctionRange *fn = functionContaining(table, rva);
            if (fn == nullptr) {
                continue;
            }
            if (const auto it = references.find(target); it != references.end()) {
                if (it->second.function != fn->root) {
                    it->second.ambiguous = true;
                }
                continue;
            }
            const std::string value = readCString(img, target, 300);
            if (value.size() < 6) {
                continue;
            }
            references.emplace(target,
                               StringReference{fn->root, static_cast<std::uint32_t>(value.size()), false});
        }
    }

    std::unordered_map<std::uint32_t, std::uint32_t> best;
    for (const auto &[string_rva, ref] : references) {
        if (ref.ambiguous || table.labels.count(ref.function) != 0) {
            continue;
        }
        const auto [it, inserted] = best.try_emplace(ref.function, string_rva);
        if (!inserted && references.at(it->second).length < ref.length) {
            it->second = string_rva;
        }
    }
    for (const auto &[function, string_rva] : best) {
        std::string value = readCString(img, string_rva, 300);
        if (value.size() > 48) {
            value.resize(45);
            value += "...";
        }
        table.labels.emplace(function, "\"" + value + "\"");
    }
}

GuessTable buildTable()
{
    GuessTable table;
    ImageView img;
    if (!img.init()) {
        return table;
    }
    collectFunctions(img, table);
    if (table.ranges.empty()) {
        return table;
    }
    collectVtableLabels(img, table);
    collectStringLabels(img, table);
    return table;
}

const GuessTable &guessTable()
{
    static const GuessTable table = buildTable();
    return table;
}

}  // namespace

std::string guessMainModuleSymbol(std::uint64_t rva)
{
    const GuessTable &table = guessTable();
    const FunctionRange *fn = functionContaining(table, rva);
    if (fn == nullptr) {
        return {};
    }
    const auto it = table.labels.find(fn->root);
    return it != table.labels.end() ? it->second : std::string();
}

}  // namespace spark

#else

namespace spark {

std::string guessMainModuleSymbol(std::uint64_t)
{
    return {};
}

}  // namespace spark

#endif
