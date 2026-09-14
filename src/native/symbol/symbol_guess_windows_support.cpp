#include "native/symbol/symbol_guess_windows_internal.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <ranges>
#include <string_view>
#include <vector>

namespace spark::symbol_guess::windows::detail {

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
    if (!mangled.starts_with(".?AV") && !mangled.starts_with(".?AU")) {
        return {};
    }
    std::string_view encoded = mangled.substr(3);
    if (!encoded.empty() && (encoded.front() == 'V' || encoded.front() == 'U')) {
        encoded.remove_prefix(1);
    }
    if (encoded.size() < 2 || !encoded.ends_with("@@")) {
        return {};
    }
    encoded.remove_suffix(2);
    if (encoded.empty()) {
        return {};
    }
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
    if (out.size() > 512) {
        return {};
    }
    return out;
}

}  // namespace spark::symbol_guess::windows::detail

namespace spark::symbol_guess::windows {

std::string Engine::Impl::readCString(std::uint32_t rva, std::uint32_t maximum) const
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

bool Engine::Impl::parseHeaders()
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
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) || nt.OptionalHeader.SizeOfImage == 0 ||
        nt.OptionalHeader.SizeOfImage > mapped_size ||
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
        if (!detail::checkedAdd(header.VirtualAddress, header.Misc.VirtualSize, end) || end > image_size ||
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
    for (std::size_t i = 1; i < sections.size(); ++i) {
        if (sections[i].begin < sections[i - 1].end) {
            sections.clear();
            return false;
        }
    }
    return !sections.empty();
}

}  // namespace spark::symbol_guess::windows

#endif
