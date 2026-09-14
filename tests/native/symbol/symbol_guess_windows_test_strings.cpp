#include "symbol_guess_windows_test_support.h"

#ifdef _WIN32

#include <cstdint>
#include <span>

namespace spark::symbol_guess::windows_test {

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
    windows::Engine engine = fixture.engine();
    const std::uint64_t query = 0x1008;
    const auto guesses = engine.guess(std::span(&query, 1));
    SPARK_SYMBOL_GUESS_CHECK(guesses.at(query).label == "str?: Server Level - tick");
    SPARK_SYMBOL_GUESS_CHECK(
        windows::scoreStringHint("Server Level - tick") >
        windows::scoreStringHint("%.2f MB of dynamic properties were saved during the last minute"));
    SPARK_SYMBOL_GUESS_CHECK(
        windows::scoreStringHint("T *Bedrock::NonOwnerPointer<ChunkPerformanceData>::_get() const") < 50);
    SPARK_SYMBOL_GUESS_CHECK(windows::scoreStringHint("Name: ") < 50);
    SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query) == guesses.at(query));
    return true;
}

bool testInstructionMiddleAndSharedString()
{
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        fixture.string(0x3100, "Level - tick redstone");
        fixture.putBytes(0x1000, {0x48, 0xb8, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0, 0xc3});
        const auto fake = static_cast<std::int32_t>(0x3100 - (0x1002 + 7));
        fixture.put(0x1005, fake);
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1002;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
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
        windows::Engine engine = fixture.engine();
        const std::uint64_t queries[] = {0x1000, 0x1100};
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(queries).empty());
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().shared_strings >= 2);
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
    windows::Engine engine = fixture.engine();
    const std::uint64_t query = 0x1110;
    SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "str?: Level - tick chained work");
    return true;
}

bool testDecodedStringLeaForms()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.string(0x3000, "Level - tick ordinary");
    fixture.string(0x3100, "Level - tick extended");
    fixture.string(0x3200, "Level - tick operand prefix");
    fixture.string(0x3300, "Level - tick extended register");
    fixture.string(0x3400, "Level - tick sixteen bit");
    fixture.string(0x3500, "Level - tick thirty two bit");
    fixture.string(0x3600, "Level - tick address override");

    fixture.runtimeFunction(0, 0x1000, 0x1010, 0x5000);
    fixture.putBytes(0x1000, {0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3});
    fixture.put(0x1003, static_cast<std::int32_t>(0x3000 - (0x1000 + 7)));

    fixture.runtimeFunction(1, 0x1100, 0x1110, 0x5000);
    fixture.putBytes(0x1100, {0x4c, 0x8d, 0x05, 0, 0, 0, 0, 0xc3});
    fixture.put(0x1103, static_cast<std::int32_t>(0x3100 - (0x1100 + 7)));

    fixture.runtimeFunction(2, 0x1200, 0x1210, 0x5000);
    fixture.putBytes(0x1200, {0x66, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3});
    fixture.put(0x1204, static_cast<std::int32_t>(0x3200 - (0x1200 + 8)));

    fixture.runtimeFunction(3, 0x1300, 0x1310, 0x5000);
    fixture.putBytes(0x1300, {0x4d, 0x8d, 0x0d, 0, 0, 0, 0, 0xc3});
    fixture.put(0x1303, static_cast<std::int32_t>(0x3300 - (0x1300 + 7)));

    fixture.runtimeFunction(4, 0x1400, 0x1410, 0x5000);
    fixture.putBytes(0x1400, {0x66, 0x8d, 0x05, 0, 0, 0, 0, 0xc3});
    fixture.put(0x1403, static_cast<std::int32_t>(0x3400 - (0x1400 + 7)));

    fixture.runtimeFunction(5, 0x1500, 0x1510, 0x5000);
    fixture.putBytes(0x1500, {0x8d, 0x05, 0, 0, 0, 0, 0xc3});
    fixture.put(0x1502, static_cast<std::int32_t>(0x3500 - (0x1500 + 6)));

    fixture.runtimeFunction(6, 0x1600, 0x1610, 0x5000);
    fixture.putBytes(0x1600, {0x67, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3});
    fixture.put(0x1604, static_cast<std::int32_t>(0x3600 - (0x1600 + 8)));

    windows::Engine engine = fixture.engine();
    const std::uint64_t queries[] = {0x1000, 0x1100, 0x1200, 0x1300, 0x1400, 0x1500, 0x1600};
    const auto guesses = engine.guess(queries);
    for (const std::uint64_t query :
         {std::uint64_t{0x1000}, std::uint64_t{0x1100}, std::uint64_t{0x1200}, std::uint64_t{0x1300}}) {
        SPARK_SYMBOL_GUESS_CHECK(guesses.at(query).label.starts_with("str?: Level - tick "));
    }
    SPARK_SYMBOL_GUESS_CHECK(!guesses.contains(0x1400));
    SPARK_SYMBOL_GUESS_CHECK(!guesses.contains(0x1500));
    SPARK_SYMBOL_GUESS_CHECK(!guesses.contains(0x1600));
    return true;
}

bool testStringOwnershipProofs()
{
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1010, 0x5000);
        fixture.runtimeFunction(1, 0x1100, 0x1120, 0x5000);
        fixture.string(0x3000, "Level - tick owner proof");
        fixture.lea(0x1000, 0x3000);
        fixture.putBytes(0x1007, {0xc3});
        fixture.putBytes(0x1100, {0x48, 0xb8, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0, 0xc3});
        fixture.put(0x1105, static_cast<std::int32_t>(0x3000 - (0x1103 + 6)));
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label ==
                                 "str?: Level - tick owner proof");
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1010, 0x5000);
        fixture.runtimeFunction(1, 0x1100, 0x1120, 0x5000);
        fixture.string(0x3000, "Level - tick owner unknown");
        fixture.lea(0x1000, 0x3000);
        fixture.putBytes(0x1007, {0xc3});
        fixture.putBytes(0x1100, {0x48, 0xb8, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0, 0xff, 0xe0});
        fixture.put(0x1105, static_cast<std::int32_t>(0x3000 - (0x1103 + 6)));
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().string_reference_ambiguities >= 1);
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x100c, 0x5000);
        fixture.runtimeFunction(1, 0x1100, 0x110f, 0x5000);
        fixture.string(0x3000, "Level - tick call tail");
        fixture.lea(0x1000, 0x3000);
        fixture.putBytes(0x1007, {0xe8, 0, 0, 0, 0});
        fixture.putBytes(0x1100, {0x48, 0xb8, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0, 0xe8, 0, 0, 0, 0});
        fixture.put(0x1105, static_cast<std::int32_t>(0x3000 - (0x1103 + 6)));
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x100c, 0x5000);
        fixture.string(0x3000, "Level - tick call tail");
        fixture.lea(0x1000, 0x3000);
        fixture.putBytes(0x1007, {0xe8, 0, 0, 0, 0});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "str?: Level - tick call tail");
    }
    return true;
}

}  // namespace spark::symbol_guess::windows_test

#endif
