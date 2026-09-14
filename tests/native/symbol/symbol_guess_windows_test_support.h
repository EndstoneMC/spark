#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_WINDOWS_TEST_SUPPORT_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_WINDOWS_TEST_SUPPORT_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "native/symbol/symbol_guess_windows.h"

namespace spark::symbol_guess::windows_test {

class PeFixture {
public:
    static constexpr std::uint64_t KBase = 0x180000000ULL;

    explicit PeFixture(std::size_t size = 0x8000, std::uint64_t load_base = KBase)
        : bytes_(size, 0), load_base_(load_base)
    {
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x80;
        put(0, dos);

        IMAGE_NT_HEADERS64 nt{};
        nt.Signature = IMAGE_NT_SIGNATURE;
        nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt.FileHeader.NumberOfSections = 4;
        nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt.OptionalHeader.ImageBase = load_base_;
        nt.OptionalHeader.SectionAlignment = 0x1000;
        nt.OptionalHeader.FileAlignment = 0x200;
        nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(size);
        nt.OptionalHeader.SizeOfHeaders = 0x400;
        nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        put(0x80, nt);

        section(0, ".text", 0x1000, 0x1000, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE);
        section(1, ".rdata", 0x2000, 0x2000, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
        section(2, ".pdata", 0x4000, 0x1000, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
        section(3, ".xdata", 0x5000, 0x1000, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    }

    template <typename T>
    void put(std::uint32_t rva, const T &value)
    {
        std::memcpy(bytes_.data() + rva, &value, sizeof(value));
    }

    void putBytes(std::uint32_t rva, std::initializer_list<std::uint8_t> bytes)
    {
        std::memcpy(bytes_.data() + rva, bytes.begin(), bytes.size());
    }

    void fillBytes(std::uint32_t rva, std::size_t count, std::uint8_t value)
    {
        std::fill(bytes_.begin() + rva, bytes_.begin() + rva + count, value);
    }

    void string(std::uint32_t rva, std::string_view value)
    {
        std::memcpy(bytes_.data() + rva, value.data(), value.size());
        bytes_[rva + value.size()] = 0;
    }

    void section(unsigned index, const char *name, std::uint32_t rva, std::uint32_t size, std::uint32_t characteristics)
    {
        IMAGE_DOS_HEADER dos{};
        std::memcpy(&dos, bytes_.data(), sizeof(dos));
        IMAGE_NT_HEADERS64 nt{};
        std::memcpy(&nt, bytes_.data() + dos.e_lfanew, sizeof(nt));
        const std::uint32_t offset = static_cast<std::uint32_t>(dos.e_lfanew) + sizeof(DWORD) +
                                     sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader +
                                     index * sizeof(IMAGE_SECTION_HEADER);
        IMAGE_SECTION_HEADER header{};
        std::memcpy(header.Name, name, std::min<std::size_t>(8, std::strlen(name)));
        header.Misc.VirtualSize = size;
        header.VirtualAddress = rva;
        header.SizeOfRawData = size;
        header.Characteristics = characteristics;
        put(offset, header);
    }

    void exceptionDirectory(std::uint32_t rva, std::uint32_t size)
    {
        IMAGE_NT_HEADERS64 nt{};
        std::memcpy(&nt, bytes_.data() + 0x80, sizeof(nt));
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = {.VirtualAddress = rva, .Size = size};
        put(0x80, nt);
    }

    void runtimeFunction(unsigned index, std::uint32_t begin, std::uint32_t end, std::uint32_t unwind)
    {
        runtimeFunctionAt(0x4000, index, begin, end, unwind);
    }

    void runtimeFunctionAt(std::uint32_t table_rva, unsigned index, std::uint32_t begin, std::uint32_t end,
                           std::uint32_t unwind)
    {
        RUNTIME_FUNCTION function{};
        function.BeginAddress = begin;
        function.EndAddress = end;
        function.UnwindData = unwind;
        put(table_rva + index * sizeof(function), function);
        exceptionDirectory(table_rva, (index + 1) * sizeof(function));
    }

    void leafUnwind(std::uint32_t rva, std::uint8_t code_count = 0) { putBytes(rva, {1, 0, code_count, 0}); }

    void chainedUnwind(std::uint32_t rva, std::uint8_t code_count, const RUNTIME_FUNCTION &parent)
    {
        putBytes(rva, {static_cast<std::uint8_t>((UNW_FLAG_CHAININFO << 3) | 1), 0, code_count, 0});
        const std::uint32_t slots = (static_cast<std::uint32_t>(code_count) + 1) & ~1U;
        put(rva + 4 + slots * 2, parent);
    }

    void typeAndCol(std::uint32_t type, std::uint32_t hierarchy, std::uint32_t base_array,
                    std::uint32_t base_descriptor, std::uint32_t col, std::string_view mangled,
                    std::uint32_t offset = 0)
    {
        string(type + 16, mangled);
        struct Hierarchy {
            std::uint32_t signature;
            std::uint32_t attributes;
            std::uint32_t count;
            std::uint32_t array;
        } chd{.signature = 0, .attributes = 0, .count = 1, .array = base_array};
        put(hierarchy, chd);
        put(base_array, base_descriptor);
        struct Base {
            std::uint32_t type;
            std::uint32_t contained;
            std::int32_t mdisp;
            std::int32_t pdisp;
            std::int32_t vdisp;
            std::uint32_t attributes;
            std::uint32_t hierarchy;
        } base{
            .type = type, .contained = 0, .mdisp = 0, .pdisp = -1, .vdisp = 0, .attributes = 0, .hierarchy = hierarchy};
        put(base_descriptor, base);
        struct Col {
            std::uint32_t signature;
            std::uint32_t offset;
            std::uint32_t cd_offset;
            std::uint32_t type;
            std::uint32_t hierarchy;
            std::uint32_t self;
        } locator{.signature = 1, .offset = offset, .cd_offset = 0, .type = type, .hierarchy = hierarchy, .self = col};
        put(col, locator);
    }

    void vtable(std::uint32_t table, std::uint32_t col, std::initializer_list<std::uint32_t> targets)
    {
        const std::uint64_t col_pointer = load_base_ + col;
        put(table - 8, col_pointer);
        unsigned slot = 0;
        for (std::uint32_t target : targets) {
            const std::uint64_t pointer = load_base_ + target;
            put(table + slot++ * 8, pointer);
        }
    }

    void lea(std::uint32_t rva, std::uint32_t target)
    {
        putBytes(rva, {0x48, 0x8d, 0x05, 0, 0, 0, 0});
        const auto displacement = static_cast<std::int32_t>(target - (rva + 7));
        put(rva + 3, displacement);
    }

    void jump(std::uint32_t rva, std::uint32_t target)
    {
        bytes_[rva] = 0xe9;
        const auto displacement = static_cast<std::int32_t>(target - (rva + 5));
        put(rva + 1, displacement);
    }

    [[nodiscard]] windows::Engine engine() const { return {bytes_.data(), bytes_.size(), load_base_}; }

    std::vector<std::uint8_t> &bytes() { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint64_t load_base_;
};

void addClass(PeFixture &fixture, std::uint32_t base, std::string_view name, std::uint32_t table,
              std::initializer_list<std::uint32_t> targets, std::uint32_t offset = 0);

#define SPARK_SYMBOL_GUESS_CHECK(condition)                                                                  \
    do {                                                                                                     \
        if (!(condition)) {                                                                                  \
            std::fprintf(stderr, "symbol guess test failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                                                    \
        }                                                                                                    \
    } while (false)

bool testPeAndFunctionRanges();
bool testChainedAndMalformedUnwind();
bool testDuplicateOverlapAndDeterminism();
bool testShortReadOnlySectionBounds();
bool testRttiVtableAmbiguity();
bool testInvalidRttiAndThunk();
bool testRttiNameCoverage();
bool testRttiNameCacheBudgetAndConcurrency();
bool testRttiNameCollisions();
bool testAslrIndependence();
bool testDecodedStringsAndScoring();
bool testDecodedStringLeaForms();
bool testStringOwnershipProofs();
bool testWindowsBudgetBoundaries();
bool testWindowsMandatoryEvidenceCases();
bool testInstructionMiddleAndSharedString();
bool testChainedRootStringUniqueness();
bool testLargeRangeLookup();
bool testSymbolGuessApplicationPolicy();
int evaluateMappedPe(int argc, char **argv);

}  // namespace spark::symbol_guess::windows_test

#endif  // ENDSTONE_SPARK_SYMBOL_GUESS_WINDOWS_TEST_SUPPORT_H
