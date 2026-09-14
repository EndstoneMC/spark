#include "symbol_guess_windows_test_support.h"

#ifdef _WIN32

#include <cstdint>
#include <span>

#include "native/symbol/symbolicate.h"

namespace spark::symbol_guess::windows_test {

bool testAslrIndependence()
{
    auto build = [](std::uint64_t load_base) {
        PeFixture fixture(0x8000, load_base);
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AVWidget@@", 0x2808, {0x1000});
        fixture.string(0x3100, "Widget - update");
        fixture.lea(0x1020, 0x3100);
        fixture.putBytes(0x1027, {0xc3});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        return engine.guess(std::span(&query, 1));
    };

    const auto preferred = build(PeFixture::KBase);
    const auto relocated = build(0x7ff600000000ULL);
    SPARK_SYMBOL_GUESS_CHECK(preferred == relocated);
    SPARK_SYMBOL_GUESS_CHECK(preferred.at(0x1000).label == "vtable: Widget::vfn[0]");
    return true;
}

bool testSymbolGuessApplicationPolicy()
{
    SPARK_SYMBOL_GUESS_CHECK(windows::guessCurrentModuleSymbols(std::span<const std::uint64_t>{}).empty());
    SPARK_SYMBOL_GUESS_CHECK(!windows::currentModuleStats().initialized);

    SPARK_SYMBOL_GUESS_CHECK(spark::frameMatchesMainModule(0x180001234, 0x1234, 0x180000000, 0x8000));
    SPARK_SYMBOL_GUESS_CHECK(!spark::frameMatchesMainModule(0x180001235, 0x1234, 0x180000000, 0x8000));
    SPARK_SYMBOL_GUESS_CHECK(!spark::frameMatchesMainModule(0x180001234, 0x9234, 0x180000000, 0x8000));
    SPARK_SYMBOL_GUESS_CHECK(!spark::frameMatchesMainModule(UINT64_MAX, 0, UINT64_MAX - 4, 16));

    spark::ResolvedFrame pdb;
    pdb.method_name = "Level::tick";
    spark::applySymbolGuessFallback(pdb, 0x1234, true, "vtable: Wrong::vfn[0]");
    SPARK_SYMBOL_GUESS_CHECK(pdb.method_name == "Level::tick");

    spark::ResolvedFrame main;
    spark::applySymbolGuessFallback(main, 0x1234, true, "vtable: Level::vfn[3]");
    SPARK_SYMBOL_GUESS_CHECK(main.method_name == "0x1234 (vtable: Level::vfn[3])");

    spark::ResolvedFrame library;
    spark::applySymbolGuessFallback(library, 0x1234, false, "vtable: Level::vfn[3]");
    SPARK_SYMBOL_GUESS_CHECK(library.method_name == "0x1234");
    return true;
}

}  // namespace spark::symbol_guess::windows_test

#endif
