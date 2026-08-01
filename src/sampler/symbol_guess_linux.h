#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_LINUX_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_LINUX_H

#if defined(__linux__) && defined(__x86_64__)

#include <cstdint>
#include <span>
#include <vector>

namespace spark::symbol_guess::linux {

// Decode reachable x86-64 instructions in one function extent and return the
// targets of RIP-relative LEA instructions. Exposed so public synthetic tests
// can prove that opcode-like bytes inside another instruction are ignored.
std::vector<std::uint64_t>
decodeRipRelativeLeaTargets(std::span<const std::uint8_t> code,
                            std::uint64_t function_rva);

} // namespace spark::symbol_guess::linux

#endif

#endif // ENDSTONE_SPARK_SYMBOL_GUESS_LINUX_H
