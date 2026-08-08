#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "native/symbol/symbol_guess_windows.h"
#include "native/symbol/symbolicate.h"

namespace {

using spark::symbol_guess::windows::Engine;
using spark::symbol_guess::windows::FunctionRange;
using spark::symbol_guess::windows::VtableEvidence;

#define CHECK(condition)                                                                                     \
    do {                                                                                                     \
        if (!(condition)) {                                                                                  \
            std::fprintf(stderr, "symbol guess test failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                                                    \
        }                                                                                                    \
    } while (false)

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
        RUNTIME_FUNCTION function{};
        function.BeginAddress = begin;
        function.EndAddress = end;
        function.UnwindData = unwind;
        put(0x4000 + index * sizeof(function), function);
        exceptionDirectory(0x4000, (index + 1) * sizeof(function));
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

    [[nodiscard]] Engine engine() const { return {bytes_.data(), bytes_.size(), load_base_}; }

    std::vector<std::uint8_t> &bytes() { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint64_t load_base_;
};

bool testPeAndFunctionRanges()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
    Engine engine = fixture.engine();
    CHECK(engine.valid());
    CHECK(engine.stats().function_ranges == 1);
    CHECK(engine.functionContaining(0x0fff) == nullptr);
    CHECK(engine.functionContaining(0x1000)->root == 0x1000);
    CHECK(engine.functionContaining(0x107f)->root == 0x1000);
    CHECK(engine.functionContaining(0x1080) == nullptr);
    CHECK(engine.functionContaining(UINT64_MAX) == nullptr);

    fixture.bytes()[0] = 0;
    CHECK(!fixture.engine().valid());
    fixture.bytes()[0] = 'M';
    fixture.bytes()[1] = 'Z';
    fixture.exceptionDirectory(0x4ffc, sizeof(RUNTIME_FUNCTION));
    CHECK(!fixture.engine().valid());
    return true;
}

bool testChainedAndMalformedUnwind()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
    RUNTIME_FUNCTION parent{};
    parent.BeginAddress = 0x1000;
    parent.EndAddress = 0x1080;
    parent.UnwindData = 0x5000;
    fixture.chainedUnwind(0x5020, 1, parent);
    fixture.runtimeFunction(1, 0x1100, 0x1180, 0x5020);
    Engine engine = fixture.engine();
    CHECK(engine.valid());
    CHECK(engine.functionContaining(0x117f)->root == 0x1000);
    CHECK(engine.stats().chained_ranges == 1);

    // A self-referential embedded chain is rejected instead of adopting an
    // arbitrary intermediate root.
    RUNTIME_FUNCTION cycle{};
    cycle.BeginAddress = 0x1200;
    cycle.EndAddress = 0x1280;
    cycle.UnwindData = 0x5040;
    fixture.chainedUnwind(0x5040, 0, cycle);
    fixture.runtimeFunction(2, 0x1200, 0x1280, 0x5040);
    Engine malformed = fixture.engine();
    CHECK(malformed.valid());
    CHECK(malformed.functionContaining(0x1210) == nullptr);
    CHECK(malformed.stats().rejected_ranges >= 1);
    return true;
}

bool testDuplicateOverlapAndDeterminism()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
    fixture.runtimeFunction(1, 0x1000, 0x1080, 0x5000);
    fixture.runtimeFunction(2, 0x1040, 0x10c0, 0x5000);
    fixture.runtimeFunction(3, 0x1100, 0x1180, 0x5000);
    Engine engine = fixture.engine();
    CHECK(engine.valid());
    CHECK(engine.functionContaining(0x1050) == nullptr);
    CHECK(engine.functionContaining(0x1110) != nullptr);
    CHECK(engine.stats().overlap_ranges == 2);
    return true;
}

void addClass(PeFixture &fixture, std::uint32_t base, std::string_view name, std::uint32_t table,
              std::initializer_list<std::uint32_t> targets, std::uint32_t offset = 0)
{
    fixture.typeAndCol(base, base + 0x100, base + 0x120, base + 0x140, base + 0x180, name, offset);
    fixture.vtable(table, base + 0x180, targets);
}

bool testRttiVtableAmbiguity()
{
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AVWidget@@", 0x2808, {0x1010});
        Engine engine = fixture.engine();
        const std::uint64_t query = 0x1010;
        CHECK(engine.guess(std::span(&query, 1)).at(query).label == "vtable: Widget::vfn[0]");
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AVWidget@@", 0x2808, {0x1010, 0x1010});
        Engine engine = fixture.engine();
        const std::uint64_t query = 0x1010;
        CHECK(engine.guess(std::span(&query, 1)).at(query).label == "vtable?: Widget::<virtual>");
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AVWidget@@", 0x2808, {0x1010});
        addClass(fixture, 0x2400, ".?AVGadget@@", 0x2c08, {0x1010});
        Engine engine = fixture.engine();
        const std::uint64_t query = 0x1010;
        CHECK(!engine.guess(std::span(&query, 1)).contains(query));
        CHECK(engine.stats().vtable_conflicts == 1);
    }
    CHECK(spark::symbol_guess::windows::chooseVtableLabel({{"Widget", 3, false, false}, {"Gadget", 3, false, false}})
              .empty());
    return true;
}

bool testInvalidRttiAndThunk()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1080, 0x10c0, 0x5000);
    // The no-pdata adjustor thunk mirrors MSVC's secondary-vftable pattern.
    fixture.putBytes(0x1000, {0x48, 0x83, 0xe9, 0x10});
    fixture.jump(0x1004, 0x1080);
    addClass(fixture, 0x2000, ".?AVChannel@@", 0x2808, {0x1000}, 16);
    Engine engine = fixture.engine();
    const std::uint64_t query = 0x1080;
    CHECK(engine.guess(std::span(&query, 1)).at(query).label == "vtable: Channel::vfn[0]");
    CHECK(engine.stats().thunk_resolved == 1);

    // Corrupt COL self pointer: the same bytes must safely produce no label.
    fixture.put<std::uint32_t>(0x2180 + 20, 0xdeadbeef);
    Engine invalid = fixture.engine();
    CHECK(invalid.guess(std::span(&query, 1)).empty());
    return true;
}

bool testAslrIndependence()
{
    auto build = [](std::uint64_t load_base) {
        PeFixture fixture(0x8000, load_base);
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AVWidget@@", 0x2808, {0x1010});
        fixture.string(0x3100, "Widget - update");
        fixture.lea(0x1020, 0x3100);
        fixture.putBytes(0x1027, {0xc3});
        Engine engine = fixture.engine();
        const std::uint64_t query = 0x1010;
        return engine.guess(std::span(&query, 1));
    };

    const auto preferred = build(PeFixture::KBase);
    const auto relocated = build(0x7ff600000000ULL);
    CHECK(preferred == relocated);
    CHECK(preferred.at(0x1010).label == "vtable: Widget::vfn[0]");
    return true;
}

bool testDecodedStringsAndScoring()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
    fixture.string(0x3000, "%.2f MB of dynamic properties were saved during the "
                           "last minute, exceeding the limit");
    fixture.string(0x3100, "Server Level - tick");
    fixture.lea(0x1000, 0x3000);
    fixture.lea(0x1007, 0x3100);
    fixture.putBytes(0x100e, {0xc3});
    Engine engine = fixture.engine();
    const std::uint64_t query = 0x1008;
    const auto guesses = engine.guess(std::span(&query, 1));
    CHECK(guesses.at(query).label == "str: Server Level - tick");
    CHECK(spark::symbol_guess::windows::scoreStringHint("Server Level - tick") >
          spark::symbol_guess::windows::scoreStringHint(
              "%.2f MB of dynamic properties were saved during the last minute"));
    CHECK(spark::symbol_guess::windows::scoreStringHint(
              "T *Bedrock::NonOwnerPointer<ChunkPerformanceData>::_get() const") < 50);
    CHECK(spark::symbol_guess::windows::scoreStringHint("Name: ") < 50);
    CHECK(engine.guess(std::span(&query, 1)).at(query) == guesses.at(query));
    return true;
}

bool testInstructionMiddleAndSharedString()
{
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        fixture.string(0x3100, "Level - tick redstone");
        // mov rax, imm64: the immediate contains a byte-perfect old-style LEA
        // pattern. A real decoder consumes it as data and must not emit an xref.
        fixture.putBytes(0x1000, {0x48, 0xb8, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0, 0xc3});
        const auto fake = static_cast<std::int32_t>(0x3100 - (0x1002 + 7));
        fixture.put(0x1005, fake);
        Engine engine = fixture.engine();
        const std::uint64_t query = 0x1002;
        CHECK(engine.guess(std::span(&query, 1)).empty());
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        fixture.runtimeFunction(1, 0x1100, 0x1180, 0x5000);
        fixture.string(0x3100, "Level - tick shared work");
        fixture.lea(0x1000, 0x3100);
        fixture.putBytes(0x1007, {0xc3});
        fixture.lea(0x1100, 0x3100);
        fixture.putBytes(0x1107, {0xc3});
        Engine engine = fixture.engine();
        const std::uint64_t queries[] = {0x1000, 0x1100};
        CHECK(engine.guess(queries).empty());
        CHECK(engine.stats().shared_strings >= 2);
    }
    return true;
}

bool testChainedRootStringUniqueness()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
    RUNTIME_FUNCTION parent{};
    parent.BeginAddress = 0x1000;
    parent.EndAddress = 0x1080;
    parent.UnwindData = 0x5000;
    fixture.chainedUnwind(0x5020, 2, parent);
    fixture.runtimeFunction(1, 0x1100, 0x1180, 0x5020);
    fixture.string(0x3100, "Level - tick chained work");
    fixture.lea(0x1000, 0x3100);
    fixture.putBytes(0x1007, {0xc3});
    fixture.lea(0x1100, 0x3100);
    fixture.putBytes(0x1107, {0xc3});
    Engine engine = fixture.engine();
    const std::uint64_t query = 0x1110;
    CHECK(engine.guess(std::span(&query, 1)).at(query).label == "str: Level - tick chained work");
    return true;
}

bool testLargeRangeLookup()
{
    constexpr std::uint32_t k_count = 120000;
    constexpr std::uint32_t k_text = 0x1000;
    constexpr std::uint32_t k_pdata = 0x200000;
    constexpr std::uint32_t k_xdata = 0x380000;
    constexpr std::uint32_t k_size = 0x390000;
    PeFixture fixture(k_size);
    fixture.section(0, ".text", k_text, k_pdata - k_text,
                    IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE);
    fixture.section(1, ".rdata", 0x100, 0x100, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.section(2, ".pdata", k_pdata, k_count * sizeof(RUNTIME_FUNCTION),
                    IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.section(3, ".xdata", k_xdata, 0x1000, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.leafUnwind(k_xdata);
    for (std::uint32_t i = 0; i < k_count; ++i) {
        RUNTIME_FUNCTION function{};
        function.BeginAddress = k_text + i * 8;
        function.EndAddress = function.BeginAddress + 4;
        function.UnwindData = k_xdata;
        fixture.put(k_pdata + i * sizeof(function), function);
    }
    fixture.exceptionDirectory(k_pdata, k_count * sizeof(RUNTIME_FUNCTION));
    Engine engine = fixture.engine();
    CHECK(engine.valid());
    CHECK(engine.stats().function_ranges == k_count);
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t checksum = 0;
    for (std::uint32_t i = 0; i < 1000000; ++i) {
        const std::uint64_t rva = k_text + (i % k_count) * 8;
        const FunctionRange *range = engine.functionContaining(rva);
        CHECK(range != nullptr);
        checksum += range->root;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(checksum != 0);
    CHECK(elapsed < std::chrono::seconds(5));
    return true;
}

bool testSymbolGuessApplicationPolicy()
{
    // Reading diagnostics must not eagerly scan the executable when every BDS
    // frame was already resolved by DbgHelp/PDB.
    CHECK(spark::symbol_guess::windows::guessCurrentModuleSymbols(std::span<const std::uint64_t>{}).empty());
    CHECK(!spark::symbol_guess::windows::currentModuleStats().initialized);

    CHECK(spark::frameMatchesMainModule(0x180001234, 0x1234, 0x180000000, 0x8000));
    CHECK(!spark::frameMatchesMainModule(0x180001235, 0x1234, 0x180000000, 0x8000));
    CHECK(!spark::frameMatchesMainModule(0x180001234, 0x9234, 0x180000000, 0x8000));
    CHECK(!spark::frameMatchesMainModule(UINT64_MAX, 0, UINT64_MAX - 4, 16));

    spark::ResolvedFrame pdb;
    pdb.method_name = "Level::tick";
    spark::applySymbolGuessFallback(pdb, 0x1234, true, "vtable: Wrong::vfn[0]");
    CHECK(pdb.method_name == "Level::tick");

    spark::ResolvedFrame main;
    spark::applySymbolGuessFallback(main, 0x1234, true, "vtable: Level::vfn[3]");
    CHECK(main.method_name == "0x1234 (vtable: Level::vfn[3])");

    spark::ResolvedFrame library;
    spark::applySymbolGuessFallback(library, 0x1234, false, "vtable: Level::vfn[3]");
    CHECK(library.method_name == "0x1234");
    return true;
}

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
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        static_cast<std::size_t>(dos.e_lfanew) > file.size() - sizeof(IMAGE_NT_HEADERS64)) {
        std::fprintf(stderr, "invalid DOS/NT headers\n");
        return 2;
    }
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, file.data() + dos.e_lfanew, sizeof(nt));
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

    Engine engine(mapped.data(), mapped.size(), nt.OptionalHeader.ImageBase);
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
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc >= 3 && std::string_view(argv[1]) == "--evaluate") {
        return evaluateMappedPe(argc, argv);
    }
    if (!testPeAndFunctionRanges() || !testChainedAndMalformedUnwind() || !testDuplicateOverlapAndDeterminism() ||
        !testRttiVtableAmbiguity() || !testInvalidRttiAndThunk() || !testAslrIndependence() ||
        !testDecodedStringsAndScoring() || !testInstructionMiddleAndSharedString() ||
        !testChainedRootStringUniqueness() || !testLargeRangeLookup() || !testSymbolGuessApplicationPolicy()) {
        return 1;
    }
    std::puts("Windows symbol guess tests passed");
    return 0;
}
