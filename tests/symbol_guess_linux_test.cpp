#include "sampler/symbol_guess_linux.h"

#if defined(__linux__) && defined(__x86_64__)

#include <array>
#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expr     \
                << '\n';                                                       \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

} // namespace

int main() {
  constexpr std::uint64_t base = 0x100;
  constexpr std::array<std::uint8_t, 8> direct = {0x48, 0x8d, 0x05, 0xf9,
                                                  0x00, 0x00, 0x00, 0xc3};
  CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(direct, base) ==
        std::vector<std::uint64_t>{0x200});

  // The LEA-looking bytes are the immediate of one MOVABS instruction and
  // must never be treated as an instruction boundary.
  constexpr std::array<std::uint8_t, 11> embedded = {
      0x48, 0xb8, 0x48, 0x8d, 0x05, 0xf2, 0x00, 0x00, 0x00, 0x90, 0xc3};
  CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(embedded, base)
            .empty());

  // Follow the taken edge of a conditional branch as well as fallthrough.
  constexpr std::array<std::uint8_t, 12> branch = {
      0x75, 0x02, 0xc3, 0x90, 0x48, 0x8d, 0x05, 0xf5, 0x00, 0x00, 0x00, 0xc3};
  CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets(branch, base) ==
        std::vector<std::uint64_t>{0x200});

  // Bytes skipped by an unconditional branch are unreachable data.
  constexpr std::array<std::uint8_t, 11> unreachable = {
      0xeb, 0x08, 0x48, 0x8d, 0x05, 0xf2, 0x00, 0x00, 0x00, 0x90, 0xc3};
  CHECK(
      spark::symbol_guess::linux::decodeRipRelativeLeaTargets(unreachable, base)
          .empty());
  CHECK(spark::symbol_guess::linux::decodeRipRelativeLeaTargets({}, base)
            .empty());

  if (failures != 0) {
    std::cerr << failures << " Linux symbol guess test(s) failed\n";
    return 1;
  }
  std::cout << "Linux symbol guess tests passed\n";
  return 0;
}

#endif
