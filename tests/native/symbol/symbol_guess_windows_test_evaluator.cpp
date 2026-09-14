#include "symbol_guess_windows_test_support.h"

#ifdef _WIN32

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace spark::symbol_guess::windows_test {

int evaluateMappedPe(int argc, char **argv)
{
    std::ifstream stream(argv[2], std::ios::binary | std::ios::ate);
    if (!stream) {
        std::fprintf(stderr, "unable to open PE: %s\n", argv[2]);
        return 2;
    }
    const std::streamoff length = stream.tellg();
    if (length <= 0 || length > 1LL << 31) {
        std::fprintf(stderr, "invalid PE size\n");
        return 2;
    }
    std::vector<std::uint8_t> file(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(file.data()), length);
    if (!stream || file.size() < sizeof(IMAGE_DOS_HEADER)) {
        std::fprintf(stderr, "unable to read PE\n");
        return 2;
    }
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, file.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        std::fprintf(stderr, "invalid DOS/NT headers\n");
        return 2;
    }
    const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    if (nt_offset > file.size() || sizeof(IMAGE_NT_HEADERS64) > file.size() - nt_offset) {
        std::fprintf(stderr, "invalid DOS/NT headers\n");
        return 2;
    }
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, file.data() + nt_offset, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage == 0 || nt.OptionalHeader.SizeOfImage > 1U << 30) {
        std::fprintf(stderr, "unsupported PE\n");
        return 2;
    }
    std::vector<std::uint8_t> mapped(nt.OptionalHeader.SizeOfImage, 0);
    const auto header_bytes = std::min<std::size_t>({file.size(), mapped.size(), nt.OptionalHeader.SizeOfHeaders});
    std::memcpy(mapped.data(), file.data(), header_bytes);
    const std::size_t section_offset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD) +
                                       sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    if (section_offset > file.size() ||
        nt.FileHeader.NumberOfSections > (file.size() - section_offset) / sizeof(IMAGE_SECTION_HEADER)) {
        std::fprintf(stderr, "invalid section table\n");
        return 2;
    }
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        std::memcpy(&section, file.data() + section_offset + i * sizeof(section), sizeof(section));
        if (section.VirtualAddress >= mapped.size() || section.PointerToRawData >= file.size()) {
            continue;
        }
        const auto copy = std::min<std::size_t>(
            {section.SizeOfRawData, file.size() - section.PointerToRawData, mapped.size() - section.VirtualAddress});
        std::memcpy(mapped.data() + section.VirtualAddress, file.data() + section.PointerToRawData, copy);
    }

    windows::Engine engine(mapped.data(), mapped.size(), nt.OptionalHeader.ImageBase);
    if (!engine.valid()) {
        std::fprintf(stderr, "symbol guess engine rejected PE\n");
        return 3;
    }
    std::vector<std::uint64_t> rvas;
    for (int i = 3; i < argc; ++i) {
        char *end = nullptr;
        const std::uint64_t rva = std::strtoull(argv[i], &end, 0);
        if (end == argv[i] || *end != 0) {
            std::fprintf(stderr, "invalid RVA: %s\n", argv[i]);
            return 2;
        }
        rvas.push_back(rva);
    }
    const auto guesses = engine.guess(rvas);
    for (std::uint64_t rva : rvas) {
        const auto it = guesses.find(rva);
        std::printf("0x%llx\t%s\n", static_cast<unsigned long long>(rva),
                    it != guesses.end() ? it->second.label.c_str() : "");
    }
    const auto stats = engine.stats();
    std::fprintf(stderr,
                 "ranges=%zu chained=%zu vtables=%zu vtable_labels=%zu thunks=%zu "
                 "sampled=%zu decoded=%zu string_labels=%zu build_us=%llu batch_us=%llu "
                 "bytes=%zu\n",
                 stats.function_ranges, stats.chained_ranges, stats.vtables, stats.vtable_labels, stats.thunk_resolved,
                 stats.sampled_functions, stats.decoded_instructions, stats.string_labels,
                 static_cast<unsigned long long>(stats.build_microseconds),
                 static_cast<unsigned long long>(stats.batch_microseconds), stats.approximate_bytes);
    std::fprintf(stderr,
                 "string_reference_candidates=%zu\nstring_reference_potential_hits=%zu\n"
                 "string_reference_exact_hits=%zu\nstring_reference_interior_rejections=%zu\n"
                 "string_reference_ambiguities=%zu\nstring_reference_shared=%zu\n"
                 "string_reference_terminal_hits_skipped=%zu\nstring_reference_unindexed=%zu\n"
                 "string_reference_unreachable=%zu\nstring_reference_overlaps=%zu\n"
                 "string_validation_functions=%zu\nstring_function_byte_budget_exhausted=%zu\n"
                 "string_function_instruction_budget_exhausted=%zu\nstring_validation_budget_exhausted=%zu\n"
                 "string_instruction_budget_exhausted=%zu\nstring_scan_byte_budget_exhausted=%zu\n"
                 "rtti_name_cache_entries=%zu\nrtti_name_cache_hits=%zu\n"
                 "rtti_name_attempts=%zu\nrtti_name_api_calls=%zu\n"
                 "rtti_name_plain=%zu\nrtti_name_complex=%zu\n"
                 "rtti_name_length_rejections=%zu\nrtti_name_failures=%zu\n"
                 "rtti_name_raw_length_rejections=%zu\n"
                 "rtti_name_output_length_rejections=%zu\n"
                 "rtti_name_collisions=%zu\nrtti_name_collision_roots=%zu\n"
                 "rtti_name_budget_exhausted=%zu\n"
                 "vtable_interior_target_rejections=%zu\nthunk_candidates=%zu\n"
                 "thunk_interior_destination_rejections=%zu\n",
                 stats.string_reference_candidates, stats.string_reference_potential_hits,
                 stats.string_reference_exact_hits, stats.string_reference_interior_rejections,
                 stats.string_reference_ambiguities, stats.string_reference_shared,
                 stats.string_reference_terminal_hits_skipped, stats.string_reference_unindexed,
                 stats.string_reference_unreachable, stats.string_reference_overlaps, stats.string_validation_functions,
                 stats.string_function_byte_budget_exhausted, stats.string_function_instruction_budget_exhausted,
                 stats.string_validation_budget_exhausted, stats.string_instruction_budget_exhausted,
                 stats.string_scan_byte_budget_exhausted, stats.rtti_name_cache_entries, stats.rtti_name_cache_hits,
                 stats.rtti_name_attempts, stats.rtti_name_api_calls, stats.rtti_name_plain, stats.rtti_name_complex,
                 stats.rtti_name_length_rejections, stats.rtti_name_raw_length_rejections,
                 stats.rtti_name_output_length_rejections, stats.rtti_name_collisions, stats.rtti_name_collision_roots,
                 stats.rtti_name_failures, stats.rtti_name_budget_exhausted, stats.vtable_interior_target_rejections,
                 stats.thunk_candidates, stats.thunk_interior_destination_rejections);
    return 0;
}

}  // namespace spark::symbol_guess::windows_test

#endif
