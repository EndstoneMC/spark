#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_LINUX_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_LINUX_H

#if defined(__linux__) && defined(__x86_64__)

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace spark::symbol_guess::linux {

struct BuildStats {
  bool initialized = false;
  std::uint64_t build_microseconds = 0;
  std::uint64_t batch_microseconds = 0;
  std::size_t image_bytes = 0;
  std::size_t table_entries = 0;
  std::size_t eh_frame_records = 0;
  std::size_t function_ranges = 0;
  std::size_t rejected_ranges = 0;
  std::size_t duplicate_ranges = 0;
  std::size_t overlap_ranges = 0;
  std::size_t unindexed_ranges = 0;
  std::size_t gap_ranges = 0;
  std::uint64_t gap_bytes = 0;
  std::size_t vtables = 0;
  std::size_t vtable_candidates = 0;
  std::size_t vtable_labels = 0;
  std::size_t vtable_conflicts = 0;
  std::size_t sampled_functions = 0;
  std::size_t decoded_instructions = 0;
  std::size_t string_candidates = 0;
  std::size_t shared_strings = 0;
  std::size_t string_labels = 0;
  std::size_t approximate_bytes = 0;
};

// Decode reachable x86-64 instructions in one function extent and return the
// targets of RIP-relative LEA instructions. Exposed so public synthetic tests
// can prove that opcode-like bytes inside another instruction are ignored.
std::vector<std::uint64_t>
decodeRipRelativeLeaTargets(std::span<const std::uint8_t> code,
                            std::uint64_t function_rva,
                            std::size_t *decoded_instructions = nullptr);

BuildStats currentModuleStats();

} // namespace spark::symbol_guess::linux

#endif

#endif // ENDSTONE_SPARK_SYMBOL_GUESS_LINUX_H
