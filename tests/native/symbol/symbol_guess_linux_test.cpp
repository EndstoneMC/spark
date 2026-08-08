#include "native/symbol/symbol_guess_linux.h"

#if defined(__linux__) && defined(__x86_64__)

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <unordered_map>

namespace {

int failures = 0;

#define CHECK(expr)                                                                       \
    do {                                                                                  \
        if (!(expr)) {                                                                    \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expr << '\n'; \
            ++failures;                                                                   \
        }                                                                                 \
    } while (false)

}  // namespace

int main()
{
    constexpr std::uint64_t base = 0x100;
    constexpr std::array<std::uint8_t, 8> direct = {0x48, 0x8d, 0x05, 0xf9, 0x00, 0x00, 0x00, 0xc3};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(direct, base) == std::vector<std::uint64_t>{0x200});

    // The LEA-looking bytes are the immediate of one MOVABS instruction and
    // must never be treated as an instruction boundary.
    constexpr std::array<std::uint8_t, 11> embedded = {0x48, 0xb8, 0x48, 0x8d, 0x05, 0xf2,
                                                       0x00, 0x00, 0x00, 0x90, 0xc3};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(embedded, base).empty());

    // Follow the taken edge of a conditional branch as well as fallthrough.
    constexpr std::array<std::uint8_t, 12> branch = {0x75, 0x02, 0xc3, 0x90, 0x48, 0x8d,
                                                     0x05, 0xf5, 0x00, 0x00, 0x00, 0xc3};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(branch, base) == std::vector<std::uint64_t>{0x200});

    // Bytes skipped by an unconditional branch are unreachable data.
    constexpr std::array<std::uint8_t, 11> unreachable = {0xeb, 0x08, 0x48, 0x8d, 0x05, 0xf2,
                                                          0x00, 0x00, 0x00, 0x90, 0xc3};
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(unreachable, base).empty());
    CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets({}, base).empty());

    constexpr std::array<std::uint8_t, 5> direct_thunk = {0xe9, 0xfb, 0x00, 0x00, 0x00};
    const auto decoded_direct = spark::symbol_guess::linux::decodeStrictThunk(direct_thunk, base);
    CHECK(decoded_direct.has_value());
    CHECK(decoded_direct->target == 0x200);
    CHECK(!decoded_direct->indirect);
    CHECK(!decoded_direct->adjusts_this);

    constexpr std::array<std::uint8_t, 9> adjustor_thunk = {0x48, 0x83, 0xc7, 0xc8, 0xe9, 0xf7, 0x00, 0x00, 0x00};
    const auto decoded_adjustor = spark::symbol_guess::linux::decodeStrictThunk(adjustor_thunk, base);
    CHECK(decoded_adjustor.has_value());
    CHECK(decoded_adjustor->target == 0x200);
    CHECK(!decoded_adjustor->indirect);
    CHECK(decoded_adjustor->adjusts_this);

    constexpr std::array<std::uint8_t, 9> lea_adjustor_thunk = {0x48, 0x8d, 0x7f, 0xc8, 0xe9, 0xf7, 0x00, 0x00, 0x00};
    const auto decoded_lea_adjustor = spark::symbol_guess::linux::decodeStrictThunk(lea_adjustor_thunk, base);
    CHECK(decoded_lea_adjustor.has_value());
    CHECK(decoded_lea_adjustor->target == 0x200);
    CHECK(decoded_lea_adjustor->adjusts_this);

    constexpr std::array<std::uint8_t, 6> got_thunk = {0xff, 0x25, 0xfa, 0x00, 0x00, 0x00};
    const auto decoded_got = spark::symbol_guess::linux::decodeStrictThunk(got_thunk, base);
    CHECK(decoded_got.has_value());
    CHECK(decoded_got->target == 0x200);
    CHECK(decoded_got->indirect);
    constexpr std::array<std::uint8_t, 3> object_dispatch = {0xff, 0x67, 0x08};
    CHECK(!spark::symbol_guess::linux::decodeStrictThunk(object_dispatch, base));

    constexpr std::array<std::uint8_t, 9> got_register_thunk = {0x48, 0x8b, 0x05, 0xf9, 0x00, 0x00, 0x00, 0xff, 0xe0};
    const auto decoded_got_register = spark::symbol_guess::linux::decodeStrictThunk(got_register_thunk, base);
    CHECK(decoded_got_register.has_value());
    CHECK(decoded_got_register->target == 0x200);
    CHECK(decoded_got_register->indirect);

    // A real wrapper has a side effect before its jump and is not a thunk.
    constexpr std::array<std::uint8_t, 8> side_effect = {0x48, 0xff, 0xc0, 0xe9, 0xf8, 0x00, 0x00, 0x00};
    CHECK(!spark::symbol_guess::linux::decodeStrictThunk(side_effect, base));
    constexpr std::array<std::uint8_t, 11> embedded_jump = {0x48, 0xb8, 0xe9, 0xfb, 0x00, 0x00,
                                                            0x00, 0x90, 0x90, 0x90, 0xc3};
    CHECK(!spark::symbol_guess::linux::decodeStrictThunk(embedded_jump, base));

    const auto edge = [](const std::unordered_map<std::uint64_t, std::uint64_t> &edges) {
        return [&edges](std::uint64_t value) -> std::optional<std::uint64_t> {
            const auto it = edges.find(value);
            return it == edges.end() ? std::nullopt : std::optional<std::uint64_t>{it->second};
        };
    };
    const std::unordered_map<std::uint64_t, std::uint64_t> two_edges = {{0x100, 0x200}, {0x200, 0x300}};
    CHECK(spark::symbol_guess::linux::followStrictThunkChain(0x100, edge(two_edges)) ==
          std::optional<std::uint64_t>{0x300});
    const std::unordered_map<std::uint64_t, std::uint64_t> loop = {{0x100, 0x200}, {0x200, 0x100}};
    CHECK(!spark::symbol_guess::linux::followStrictThunkChain(0x100, edge(loop)));
    const std::unordered_map<std::uint64_t, std::uint64_t> too_deep = {{0x100, 0x200}, {0x200, 0x300}, {0x300, 0x400}};
    CHECK(!spark::symbol_guess::linux::followStrictThunkChain(0x100, edge(too_deep)));

    if (failures != 0) {
        std::cerr << failures << " Linux symbol guess test(s) failed\n";
        return 1;
    }
    std::cout << "Linux symbol guess tests passed\n";
    return 0;
}

#endif
