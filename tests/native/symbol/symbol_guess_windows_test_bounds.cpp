#include "symbol_guess_windows_test_support.h"

#ifdef _WIN32

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spark::symbol_guess::windows_test {

namespace {

constexpr std::uint32_t kLargeText = 0xc0000;
constexpr std::uint32_t kLargeRdata = 0x2000;
constexpr std::uint32_t kLargePdata = 0x90000;
constexpr std::uint32_t kLargeXdata = 0xb0000;

void layoutLargeFixture(PeFixture &fixture, std::uint32_t text_size, std::uint32_t rdata_size, std::uint32_t pdata_size)
{
    fixture.section(0, ".text", kLargeText, text_size, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE);
    fixture.section(1, ".rdata", kLargeRdata, rdata_size, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.section(2, ".pdata", kLargePdata, pdata_size, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.section(3, ".xdata", kLargeXdata, 0x1000, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.leafUnwind(kLargeXdata);
}

PeFixture makeValidationFixture(std::size_t root_count, std::size_t base_instruction_count = 2,
                                std::size_t extra_instruction_roots = 0)
{
    constexpr std::size_t k_stride = 0x120;
    const std::uint32_t text_size = static_cast<std::uint32_t>(0x1000 + root_count * k_stride);
    const std::uint32_t rdata_size = static_cast<std::uint32_t>(0x2000 + root_count * 32);
    const std::uint32_t pdata_size = static_cast<std::uint32_t>(0x1000 + root_count * sizeof(RUNTIME_FUNCTION));
    PeFixture fixture(0x200000);
    layoutLargeFixture(fixture, text_size, rdata_size, pdata_size);
    for (std::size_t i = 0; i < root_count; ++i) {
        const auto root = kLargeText + static_cast<std::uint32_t>(i * k_stride);
        const auto target = kLargeRdata + 0x1000 + static_cast<std::uint32_t>(i * 32);
        const std::size_t instruction_count = base_instruction_count + (i < extra_instruction_roots ? 1 : 0);
        fixture.runtimeFunctionAt(kLargePdata, static_cast<unsigned>(i), root,
                                  root + static_cast<std::uint32_t>(7 + instruction_count - 1), kLargeXdata);
        fixture.string(target, "Level - tick root " + std::to_string(i));
        fixture.lea(root, target);
        fixture.fillBytes(root + 7, instruction_count - 2, 0x90);
        fixture.putBytes(root + static_cast<std::uint32_t>(7 + instruction_count - 2), {0xc3});
    }
    return fixture;
}

std::vector<std::uint64_t> roots(std::size_t count)
{
    std::vector<std::uint64_t> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(kLargeText + static_cast<std::uint32_t>(i * 0x120));
    }
    return out;
}

bool sameBudgetStats(const windows::BuildStats &a, const windows::BuildStats &b)
{
    return a.string_validation_functions == b.string_validation_functions &&
           a.string_function_byte_budget_exhausted == b.string_function_byte_budget_exhausted &&
           a.string_function_instruction_budget_exhausted == b.string_function_instruction_budget_exhausted &&
           a.string_validation_budget_exhausted == b.string_validation_budget_exhausted &&
           a.string_instruction_budget_exhausted == b.string_instruction_budget_exhausted &&
           a.string_scan_byte_budget_exhausted == b.string_scan_byte_budget_exhausted;
}

void putImmediateFalseLea(PeFixture &fixture, std::uint32_t rva, std::uint32_t target)
{
    fixture.putBytes(rva, {0x48, 0xb8, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0});
    fixture.put(rva + 5, static_cast<std::int32_t>(target - (rva + 3 + 6)));
}

void putVariant(PeFixture &fixture, std::uint32_t rva, std::uint32_t target, unsigned variant)
{
    switch (variant) {
    case 0:
        fixture.putBytes(rva, {0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3});
        fixture.put(rva + 3, static_cast<std::int32_t>(target - (rva + 7)));
        break;
    case 1:
        fixture.putBytes(rva, {0x4c, 0x8d, 0x05, 0, 0, 0, 0, 0xc3});
        fixture.put(rva + 3, static_cast<std::int32_t>(target - (rva + 7)));
        break;
    case 2:
        fixture.putBytes(rva, {0x66, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3});
        fixture.put(rva + 4, static_cast<std::int32_t>(target - (rva + 8)));
        break;
    default:
        fixture.putBytes(rva, {0x4d, 0x8d, 0x0d, 0, 0, 0, 0, 0xc3});
        fixture.put(rva + 3, static_cast<std::int32_t>(target - (rva + 7)));
        break;
    }
}

}  // namespace

bool testWindowsBudgetBoundaries()
{
    {
        for (const bool over : {false, true}) {
            PeFixture fixture(0x30000);
            fixture.section(0, ".text", 0x6000, 0x20000,
                            IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE);
            fixture.leafUnwind(0x5000);
            fixture.string(0x3000, "Level - tick decoded bytes");
            constexpr std::size_t k_mov_count = 6552;
            const std::uint32_t root = 0x6000;
            const std::size_t expected_bytes = windows::kMaximumFunctionDecodeBytes + (over ? 1U : 0U);
            fixture.runtimeFunction(0, root, root + static_cast<std::uint32_t>(expected_bytes), 0x5000);
            fixture.lea(root, 0x3000);
            std::uint32_t cursor = root + 7;
            for (std::size_t i = 0; i < k_mov_count; ++i) {
                fixture.putBytes(cursor, {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0});
                cursor += 10;
            }
            fixture.putBytes(cursor, {0x48, 0x83, 0xc0, 0, 0x48, 0x83, 0xc0, 0});
            cursor += 8;
            if (over) {
                fixture.putBytes(cursor, {0x90});
                ++cursor;
            }
            fixture.putBytes(cursor, {0xc3});
            ++cursor;
            SPARK_SYMBOL_GUESS_CHECK(cursor - root == expected_bytes);
            windows::Engine engine = fixture.engine();
            const std::uint64_t query = root;
            const auto guesses = engine.guess(std::span(&query, 1));
            if (over) {
                SPARK_SYMBOL_GUESS_CHECK(guesses.empty());
                SPARK_SYMBOL_GUESS_CHECK(engine.stats().string_function_byte_budget_exhausted >= 1);
            }
            else {
                SPARK_SYMBOL_GUESS_CHECK(guesses.at(query).label == "str?: Level - tick decoded bytes");
                SPARK_SYMBOL_GUESS_CHECK(engine.stats().string_function_byte_budget_exhausted == 0);
            }
        }
    }
    {
        for (const bool over : {false, true}) {
            PeFixture fixture(0x30000);
            fixture.section(0, ".text", 0x6000, 0x20000,
                            IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE);
            fixture.leafUnwind(0x5000);
            fixture.string(0x3000, "Level - tick decoded instructions");
            constexpr std::size_t k_instructions = 8192;
            const std::size_t instruction_count = k_instructions + (over ? 1U : 0U);
            const std::uint32_t root = 0x6000;
            fixture.runtimeFunction(0, root, root + static_cast<std::uint32_t>(7 + instruction_count - 1), 0x5000);
            fixture.lea(root, 0x3000);
            fixture.fillBytes(root + 7, instruction_count - 2, 0x90);
            fixture.putBytes(root + static_cast<std::uint32_t>(7 + instruction_count - 2), {0xc3});
            windows::Engine engine = fixture.engine();
            const std::uint64_t query = root;
            const auto guesses = engine.guess(std::span(&query, 1));
            if (over) {
                SPARK_SYMBOL_GUESS_CHECK(guesses.empty());
                SPARK_SYMBOL_GUESS_CHECK(engine.stats().string_function_instruction_budget_exhausted >= 1);
            }
            else {
                SPARK_SYMBOL_GUESS_CHECK(guesses.at(query).label == "str?: Level - tick decoded instructions");
                SPARK_SYMBOL_GUESS_CHECK(engine.stats().string_function_instruction_budget_exhausted == 0);
            }
        }
    }
    {
        for (const std::size_t count :
             {windows::kMaximumBatchFunctionValidations, windows::kMaximumBatchFunctionValidations + 1U}) {
            PeFixture fixture = makeValidationFixture(count);
            windows::Engine engine = fixture.engine();
            const auto query = roots(count);
            const auto guesses = engine.guess(query);
            const auto stats = engine.stats();
            if (count == windows::kMaximumBatchFunctionValidations) {
                SPARK_SYMBOL_GUESS_CHECK(guesses.size() == count);
                SPARK_SYMBOL_GUESS_CHECK(stats.string_validation_functions == count);
                SPARK_SYMBOL_GUESS_CHECK(stats.string_validation_budget_exhausted == 0);
            }
            else {
                SPARK_SYMBOL_GUESS_CHECK(guesses.size() == windows::kMaximumBatchFunctionValidations);
                SPARK_SYMBOL_GUESS_CHECK(stats.string_validation_functions ==
                                         windows::kMaximumBatchFunctionValidations);
                SPARK_SYMBOL_GUESS_CHECK(stats.string_validation_budget_exhausted >= 1);
                auto reversed = query;
                std::ranges::reverse(reversed);
                const auto reversed_guesses = engine.guess(reversed);
                const auto reversed_stats = engine.stats();
                SPARK_SYMBOL_GUESS_CHECK(reversed_guesses == guesses);
                SPARK_SYMBOL_GUESS_CHECK(sameBudgetStats(stats, reversed_stats));
            }
        }
    }
    {
        for (const bool over : {false, true}) {
            constexpr std::size_t k_roots = windows::kMaximumBatchFunctionValidations;
            const std::size_t extra = over ? 577U : 576U;
            PeFixture fixture = makeValidationFixture(k_roots, 244, extra);
            windows::Engine engine = fixture.engine();
            const auto query = roots(k_roots);
            const auto guesses = engine.guess(query);
            const auto stats = engine.stats();
            SPARK_SYMBOL_GUESS_CHECK(stats.decoded_instructions == windows::kMaximumBatchDecodedInstructions);
            if (over) {
                SPARK_SYMBOL_GUESS_CHECK(stats.string_instruction_budget_exhausted >= 1);
            }
            else {
                SPARK_SYMBOL_GUESS_CHECK(stats.string_instruction_budget_exhausted == 0);
                SPARK_SYMBOL_GUESS_CHECK(guesses.size() == k_roots);
            }
        }
    }
    {
        constexpr std::uint32_t text_rva = 0x6000;
        const auto scan_limit = static_cast<std::uint32_t>(windows::kMaximumBatchExecutableScanBytes);
        const std::uint32_t rdata_rva = text_rva + scan_limit + 0x1000;
        PeFixture fixture(static_cast<std::size_t>(rdata_rva) + 0x4000);
        fixture.section(0, ".text", text_rva, scan_limit,
                        IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE);
        fixture.section(1, ".rdata", rdata_rva, 0x2000, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
        fixture.leafUnwind(0x5000);
        fixture.string(rdata_rva, "Level - tick executable scan");
        fixture.runtimeFunction(0, text_rva, text_rva + 8, 0x5000);
        fixture.lea(text_rva, rdata_rva);
        fixture.putBytes(text_rva + 7, {0xc3});
        {
            windows::Engine exact = fixture.engine();
            const std::uint64_t query = text_rva;
            SPARK_SYMBOL_GUESS_CHECK(exact.guess(std::span(&query, 1)).at(query).label ==
                                     "str?: Level - tick executable scan");
            SPARK_SYMBOL_GUESS_CHECK(exact.stats().string_scan_byte_budget_exhausted == 0);
        }
        fixture.section(0, ".text", text_rva, scan_limit + 1U,
                        IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE);
        windows::Engine over = fixture.engine();
        const std::uint64_t query = text_rva;
        SPARK_SYMBOL_GUESS_CHECK(over.guess(std::span(&query, 1)).empty());
        SPARK_SYMBOL_GUESS_CHECK(over.stats().string_scan_byte_budget_exhausted >= 1);
    }
    return true;
}

bool testWindowsMandatoryEvidenceCases()
{
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.string(0x3000, "Level - tick boundary call");
        fixture.runtimeFunction(0, 0x1000, 0x1005, 0x5000);
        RUNTIME_FUNCTION parent{.BeginAddress = 0x1000, .EndAddress = 0x1005, .UnwindData = 0x5000};
        fixture.chainedUnwind(0x5020, 0, parent);
        fixture.runtimeFunction(1, 0x1100, 0x1110, 0x5020);
        fixture.putBytes(0x1000, {0xe8, 0, 0, 0, 0});
        fixture.lea(0x1100, 0x3000);
        fixture.putBytes(0x1107, {0xc3});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1108;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label ==
                                 "str?: Level - tick boundary call");
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.string(0x3000, "Level - tick same owner");
        fixture.runtimeFunction(0, 0x1000, 0x1022, 0x5000);
        putImmediateFalseLea(fixture, 0x1000, 0x3000);
        fixture.lea(0x100a, 0x3000);
        fixture.putBytes(0x1011, {0xff, 0xe0});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "str?: Level - tick same owner");
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.string(0x3000, "Level - tick branch overlap");
        fixture.runtimeFunction(0, 0x1000, 0x1020, 0x5000);
        fixture.lea(0x1000, 0x3000);
        putImmediateFalseLea(fixture, 0x1007, 0x3000);
        fixture.jump(0x1017, 0x1009);
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.string(0x3000, "Level - tick decode failure");
        fixture.runtimeFunction(0, 0x1000, 0x1008, 0x5000);
        fixture.lea(0x1000, 0x3000);
        fixture.putBytes(0x1007, {0x0f});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
    }
    {
        for (const std::uint8_t terminator : {std::uint8_t{0x0f}, std::uint8_t{0xcd}, std::uint8_t{0xf4}}) {
            PeFixture fixture;
            fixture.leafUnwind(0x5000);
            fixture.string(0x3000, "Level - tick hard terminator");
            fixture.runtimeFunction(0, 0x1000, 0x1010, 0x5000);
            fixture.lea(0x1000, 0x3000);
            if (terminator == 0x0f) {
                fixture.putBytes(0x1007, {0x0f, 0x05});
            }
            else if (terminator == 0xcd) {
                fixture.putBytes(0x1007, {0xcd, 0x80});
            }
            else {
                fixture.putBytes(0x1007, {0xf4});
            }
            windows::Engine engine = fixture.engine();
            const std::uint64_t query = 0x1000;
            SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
        }
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.string(0x3000, "Level - tick missing primary");
        RUNTIME_FUNCTION parent{.BeginAddress = 0x1000, .EndAddress = 0x1010, .UnwindData = 0x5000};
        fixture.chainedUnwind(0x5020, 0, parent);
        fixture.runtimeFunction(0, 0x1100, 0x1110, 0x5020);
        fixture.lea(0x1100, 0x3000);
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1100;
        SPARK_SYMBOL_GUESS_CHECK(engine.functionContaining(query)->root == 0x1000);
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.string(0x3000, "Level - tick ambiguous primary");
        fixture.runtimeFunction(0, 0x1000, 0x1010, 0x5000);
        fixture.runtimeFunction(1, 0x1008, 0x1018, 0x5000);
        RUNTIME_FUNCTION parent{.BeginAddress = 0x1000, .EndAddress = 0x1010, .UnwindData = 0x5000};
        fixture.chainedUnwind(0x5020, 0, parent);
        fixture.runtimeFunction(2, 0x1100, 0x1110, 0x5020);
        fixture.lea(0x1100, 0x3000);
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1100;
        SPARK_SYMBOL_GUESS_CHECK(engine.functionContaining(query)->root == 0x1000);
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.string(0x3000, "Level worker task");
        fixture.runtimeFunction(0, 0x1000, 0x1010, 0x5000);
        fixture.lea(0x1000, 0x3000);
        fixture.putBytes(0x1007, {0xc3});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        const auto label = windows::formatStringHint("Level worker task");
        SPARK_SYMBOL_GUESS_CHECK(label.kind == GuessKind::String);
        SPARK_SYMBOL_GUESS_CHECK(label.confidence == Confidence::Medium);
        SPARK_SYMBOL_GUESS_CHECK(label.label == "str?: Level worker task");
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query) == label);
    }
    {
        for (unsigned variant = 0; variant < 4; ++variant) {
            const auto target = 0x3000U + variant * 0x100U;
            PeFixture fixture;
            fixture.leafUnwind(0x5000);
            fixture.string(target, "Level - tick admitted variant " + std::to_string(variant));
            const auto primary = 0x1000U + variant * 0x200U;
            const auto second = primary + 0x80U;
            fixture.runtimeFunction(variant * 2, primary, primary + 0x20, 0x5000);
            fixture.runtimeFunction(variant * 2 + 1, second, second + 0x20, 0x5000);
            putVariant(fixture, primary, target, variant);
            putVariant(fixture, second, target, variant);
            windows::Engine engine = fixture.engine();
            const std::uint64_t query = 0x1000U + variant * 0x200U;
            SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
        }
    }
    return true;
}

}  // namespace spark::symbol_guess::windows_test

#endif
