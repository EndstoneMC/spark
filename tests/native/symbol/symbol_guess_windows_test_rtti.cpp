#include "symbol_guess_windows_test_support.h"

#ifdef _WIN32

#include <cstdint>
#include <span>

namespace spark::symbol_guess::windows_test {

bool testRttiVtableAmbiguity()
{
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AVWidget@@", 0x2808, {0x1010});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1010;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "vtable: Widget::vfn[0]");
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AVWidget@@", 0x2808, {0x1010, 0x1010});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1010;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "vtable?: Widget::<virtual>");
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AVWidget@@", 0x2808, {0x1010});
        addClass(fixture, 0x2400, ".?AVGadget@@", 0x2c08, {0x1010});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1010;
        SPARK_SYMBOL_GUESS_CHECK(!engine.guess(std::span(&query, 1)).contains(query));
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().vtable_conflicts == 1);
    }
    SPARK_SYMBOL_GUESS_CHECK(
        windows::chooseVtableLabel({{"Widget", 3, false, false}, {"Gadget", 3, false, false}}).empty());
    return true;
}

bool testInvalidRttiAndThunk()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1080, 0x10c0, 0x5000);
    fixture.putBytes(0x1000, {0x48, 0x83, 0xe9, 0x10});
    fixture.jump(0x1004, 0x1080);
    addClass(fixture, 0x2000, ".?AVChannel@@", 0x2808, {0x1000}, 16);
    windows::Engine engine = fixture.engine();
    const std::uint64_t query = 0x1080;
    SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "vtable?: Channel::<virtual>");
    SPARK_SYMBOL_GUESS_CHECK(engine.stats().thunk_resolved == 1);

    fixture.put<std::uint32_t>(0x2180 + 20, 0xdeadbeef);
    windows::Engine invalid = fixture.engine();
    SPARK_SYMBOL_GUESS_CHECK(invalid.guess(std::span(&query, 1)).empty());
    return true;
}

}  // namespace spark::symbol_guess::windows_test

#endif
