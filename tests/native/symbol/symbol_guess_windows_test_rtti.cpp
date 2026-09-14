#include "symbol_guess_windows_test_support.h"

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "native/symbol/symbolicate.h"

namespace spark::symbol_guess::windows_test {

bool testRttiVtableAmbiguity()
{
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AV?$Box@H@@", 0x2808, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "vtable: Box<int>::vfn[0]");
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_attempts == 1);
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_api_calls == 1);
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_complex == 1);
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AVWidget@@", 0x2808, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "vtable: Widget::vfn[0]");
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AVWidget@@", 0x2808, {0x1000, 0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "vtable?: Widget::<virtual>");
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AVWidget@@", 0x2808, {0x1000});
        addClass(fixture, 0x2400, ".?AVGadget@@", 0x2c08, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
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

    {
        PeFixture known;
        known.leafUnwind(0x5000);
        known.runtimeFunction(0, 0x1000, 0x1010, 0x5000);
        known.runtimeFunction(1, 0x1080, 0x10c0, 0x5000);
        known.jump(0x1000, 0x1080);
        addClass(known, 0x2000, ".?AVKnownThunk@@", 0x2808, {0x1000});
        windows::Engine known_engine = known.engine();
        const std::uint64_t known_query = 0x1080;
        SPARK_SYMBOL_GUESS_CHECK(known_engine.guess(std::span(&known_query, 1)).at(known_query).label ==
                                 "vtable?: KnownThunk::<virtual>");
    }
    {
        PeFixture interior;
        interior.leafUnwind(0x5000);
        interior.runtimeFunction(0, 0x1000, 0x1010, 0x5000);
        interior.runtimeFunction(1, 0x1080, 0x10c0, 0x5000);
        interior.jump(0x1000, 0x1080);
        addClass(interior, 0x2000, ".?AVInteriorThunk@@", 0x2808, {0x1004});
        windows::Engine interior_engine = interior.engine();
        const std::uint64_t interior_query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(interior_engine.guess(std::span(&interior_query, 1)).empty());
    }
    {
        PeFixture interior_destination;
        interior_destination.leafUnwind(0x5000);
        interior_destination.runtimeFunction(0, 0x1000, 0x1010, 0x5000);
        interior_destination.runtimeFunction(1, 0x1080, 0x10c0, 0x5000);
        interior_destination.jump(0x1000, 0x1084);
        addClass(interior_destination, 0x2000, ".?AVInteriorDestination@@", 0x2808, {0x1000});
        windows::Engine destination_engine = interior_destination.engine();
        const std::uint64_t destination_query = 0x1080;
        SPARK_SYMBOL_GUESS_CHECK(destination_engine.guess(std::span(&destination_query, 1)).empty());
    }
    return true;
}

bool testRttiNameCoverage()
{
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AURecord@Inner@Outer@@", 0x2808, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label ==
                                 "vtable: Outer::Inner::Record::vfn[0]");
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AV?$Box@H@@", 0x2808, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "vtable: Box<int>::vfn[0]");
    }

    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, ".?AV?$Outer@V?$Inner@H@@@@", 0x2808, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label ==
                                 "vtable: Outer<Inner<int> >::vfn[0]");
    }
    {
        const std::string raw = ".?AVA@?A0x12345678@@";
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, raw, 0x2808, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label ==
                                 "vtable: `anonymous namespace'::A::vfn[0]");
    }
    {
        const std::string raw = ".?AVLocal@?1??foo@@YAXXZ@";
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, raw, 0x2808, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label ==
                                 "vtable: `foo'::`2'::Local::vfn[0]");
    }
    {
        const std::string raw = ".?AV<lambda_0>@?0??foo@@YAXXZ@";
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, raw, 0x2808, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label ==
                                 "vtable: `foo'::`1'::<lambda_0>::vfn[0]");
    }
    {
        const std::string raw = ".?AV<lambda_1>@?1?foo@@YAXXZ@@";
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, raw, 0x2808, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
    }

    const auto rejected = [](std::string name) {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, name, 0x2808, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        return engine.guess(std::span(&query, 1)).empty();
    };
    for (const std::string &name :
         {std::string(".??AVWidget@@"), std::string(".?AXWidget@@"), std::string(".?AVWidget@"),
          std::string(".?AVWidget@@trailing"), std::string(".?AV@@"), std::string(".?AVWidget\x01@@")}) {
        SPARK_SYMBOL_GUESS_CHECK(rejected(name));
    }
    SPARK_SYMBOL_GUESS_CHECK(rejected(std::string(".?AVWidget") + '\0' + "@@"));
    {
        std::string exact = ".?AV" + std::string(512, 'A') + "@@";
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        fixture.typeAndCol(0x2000, 0x2800, 0x2900, 0x2a00, 0x2b00, exact);
        fixture.vtable(0x2c08, 0x2b00, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        const auto exact_guesses = engine.guess(std::span(&query, 1));
        SPARK_SYMBOL_GUESS_CHECK(!exact_guesses.empty());
    }
    {
        std::string over = ".?AV" + std::string(513, 'A') + "@@";
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        fixture.typeAndCol(0x2000, 0x2800, 0x2900, 0x2a00, 0x2b00, over);
        fixture.vtable(0x2c08, 0x2b00, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_length_rejections >= 1);
    }
    for (const std::size_t letters : {1018U, 1019U}) {
        std::string raw = ".?AV" + std::string(letters, 'A') + "@@";
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        fixture.typeAndCol(0x2000, 0x2800, 0x2900, 0x2a00, 0x2b00, raw);
        fixture.vtable(0x2c08, 0x2b00, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_length_rejections >= 1);
        if (letters == 1018U) {
            SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_raw_length_rejections == 0);
            SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_output_length_rejections >= 1);
        }
        else {
            SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_raw_length_rejections >= 1);
        }
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        fixture.typeAndCol(0x2000, 0x2100, 0x2120, 0x2140, 0x2180, ".?AV?$Bad@@");
        fixture.vtable(0x2808, 0x2180, {0x1000});
        fixture.vtable(0x2a08, 0x2180, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_attempts == 1);
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_cache_hits >= 1);
    }
    return true;
}

bool testRttiNameCacheBudgetAndConcurrency()
{
    constexpr std::size_t k_count = 4097;
    constexpr std::uint32_t k_rdata = 0x2000;
    constexpr std::uint32_t k_pdata = 0x820000;
    constexpr std::uint32_t k_xdata = 0x830000;
    constexpr std::uint32_t k_text = 0x840000;
    PeFixture fixture(0x880000);
    fixture.section(0, ".text", k_text, 0x20000, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE);
    fixture.section(1, ".rdata", k_rdata, 0x810000, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.section(2, ".pdata", k_pdata, 0x10000, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.section(3, ".xdata", k_xdata, 0x1000, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.leafUnwind(k_xdata);
    for (std::size_t i = 0; i < k_count; ++i) {
        const auto base = 0x3000U + static_cast<std::uint32_t>(i * 0x200U);
        const auto root = k_text + static_cast<std::uint32_t>(i * 16U);
        fixture.runtimeFunctionAt(k_pdata, static_cast<unsigned>(i), root, root + 8, k_xdata);
        fixture.putBytes(root, {0xc3});
        addClass(fixture, base, ".?AVClass" + std::to_string(i) + "@@", base + 0x208, {root});
    }
    windows::Engine engine = fixture.engine();
    const std::uint64_t query = k_text;
    SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "vtable: Class0::vfn[0]");
    const auto stats = engine.stats();
    SPARK_SYMBOL_GUESS_CHECK(stats.rtti_name_attempts == 4096);
    SPARK_SYMBOL_GUESS_CHECK(stats.rtti_name_cache_entries == 4096);
    SPARK_SYMBOL_GUESS_CHECK(stats.rtti_name_budget_exhausted >= 1);

    PeFixture concurrent_fixture;
    concurrent_fixture.leafUnwind(0x5000);
    concurrent_fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
    addClass(concurrent_fixture, 0x2000, ".?AV?$Box@H@@", 0x2808, {0x1000});
    const std::uint64_t concurrent_query = 0x1000;
    std::atomic<bool> failed = false;
    std::vector<std::thread> workers;
    workers.reserve(5);
    for (unsigned i = 0; i < 4; ++i) {
        workers.emplace_back([&]() {
            for (unsigned repeat = 0; repeat < 8 && !failed.load(); ++repeat) {
                windows::Engine concurrent = concurrent_fixture.engine();
                const auto result = concurrent.guess(std::span(&concurrent_query, 1));
                if (result.empty() || result.at(concurrent_query).label != "vtable: Box<int>::vfn[0]") {
                    failed.store(true);
                }
            }
        });
    }
    workers.emplace_back([&]() {
        spark::ModuleTable modules;
        const auto module = modules.intern("rtti-concurrency.exe");
        const spark::FrameKey unresolved_nonmain{.module = module, .rva = 0x42, .raw_address = 0};
        const auto fallback = spark::resolveFrames(modules, std::vector<spark::FrameKey>{unresolved_nonmain});
        if (fallback.empty() || fallback.at(unresolved_nonmain).method_name != "0x42") {
            failed.store(true);
        }
        const spark::FrameKey key{.module = module,
                                  .rva = 0,
                                  .raw_address =
                                      reinterpret_cast<std::uint64_t>(&testRttiNameCacheBudgetAndConcurrency)};
        for (unsigned repeat = 0; repeat < 32 && !failed.load(); ++repeat) {
            if (spark::resolveFrames(modules, std::vector<spark::FrameKey>{key}).empty()) {
                failed.store(true);
            }
        }
    });
    for (auto &worker : workers) {
        worker.join();
    }
    SPARK_SYMBOL_GUESS_CHECK(!failed.load());
    return true;
}

bool testRttiNameCollisions()
{
    const std::string local_int = ".?AVLocal@?1??foo@@YAXH@Z@";
    const std::string local_float = ".?AVLocal@?1??foo@@YAXM@Z@";
    for (const bool reverse : {false, true}) {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        if (reverse) {
            addClass(fixture, 0x2000, local_float, 0x2808, {0x1000});
            addClass(fixture, 0x2400, local_int, 0x2c08, {0x1000});
        }
        else {
            addClass(fixture, 0x2000, local_int, 0x2808, {0x1000});
            addClass(fixture, 0x2400, local_float, 0x2c08, {0x1000});
        }
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_collisions >= 1);
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_collision_roots >= 1);
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        addClass(fixture, 0x2000, local_int, 0x2808, {0x1000});
        addClass(fixture, 0x2400, local_int, 0x2c08, {0x1000});
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1000;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label ==
                                 "vtable: `foo'::`2'::Local::vfn[0]");
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_collisions == 0);
    }
    for (const bool valid_first : {false, true}) {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        fixture.runtimeFunction(1, 0x1100, 0x1180, 0x5000);
        auto add_valid = [&]() {
            addClass(fixture, 0x2000, ".?AVValid@@", 0x2808, {0x1100});
        };
        auto add_collision = [&]() {
            addClass(fixture, 0x2400, local_int, 0x2c08, {0x1000});
            addClass(fixture, 0x2800, local_float, 0x3008, {0x1000});
        };
        if (valid_first) {
            add_valid();
            add_collision();
        }
        else {
            add_collision();
            add_valid();
        }
        windows::Engine engine = fixture.engine();
        const std::uint64_t queries[] = {0x1000, 0x1100};
        const auto guesses = engine.guess(queries);
        SPARK_SYMBOL_GUESS_CHECK(!guesses.contains(0x1000));
        SPARK_SYMBOL_GUESS_CHECK(guesses.at(0x1100).label == "vtable: Valid::vfn[0]");
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        fixture.runtimeFunction(1, 0x1100, 0x1180, 0x5000);
        addClass(fixture, 0x2000, local_int, 0x2808, {0x1000});
        addClass(fixture, 0x2400, local_float, 0x2c08, {0x1000});
        addClass(fixture, 0x3000, local_int, 0x3208, {0x1100});
        addClass(fixture, 0x3400, ".?AVValid@@", 0x3608, {0x1100});
        windows::Engine engine = fixture.engine();
        const std::uint64_t queries[] = {0x1000, 0x1100};
        const auto guesses = engine.guess(queries);
        SPARK_SYMBOL_GUESS_CHECK(guesses.empty());
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().rtti_name_collision_roots == 2);
    }
    return true;
}

}  // namespace spark::symbol_guess::windows_test

#endif
