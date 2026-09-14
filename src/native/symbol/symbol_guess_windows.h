#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_WINDOWS_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_WINDOWS_H

#if defined(_WIN32)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "native/symbol/symbol_guess_evidence.h"

namespace spark::symbol_guess::windows {

inline constexpr std::size_t kMaximumFunctionDecodeBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumFunctionDecodeInstructions = 8192U;
inline constexpr std::size_t kMaximumBatchFunctionValidations = 4096U;
inline constexpr std::size_t kMaximumBatchDecodedInstructions = 1000000U;
inline constexpr std::size_t kMaximumBatchExecutableScanBytes = 256U * 1024U * 1024U;

struct FunctionRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::uint32_t root = 0;

    bool operator==(const FunctionRange &) const = default;
};

using VtableEvidence = ::spark::symbol_guess::VtableEvidence;
using TypedLabel = ::spark::symbol_guess::TypedLabel;

struct BuildStats {
    bool initialized = false;
    std::uint64_t build_microseconds = 0;
    std::uint64_t batch_microseconds = 0;
    std::size_t image_bytes = 0;
    std::size_t function_ranges = 0;
    std::size_t chained_ranges = 0;
    std::size_t rejected_ranges = 0;
    std::size_t overlap_ranges = 0;
    std::size_t vtables = 0;
    std::size_t vtable_candidates = 0;
    std::size_t vtable_labels = 0;
    std::size_t vtable_conflicts = 0;
    std::size_t rtti_name_cache_entries = 0;
    std::size_t rtti_name_cache_hits = 0;
    std::size_t rtti_name_attempts = 0;
    std::size_t rtti_name_api_calls = 0;
    std::size_t rtti_name_plain = 0;
    std::size_t rtti_name_complex = 0;
    std::size_t rtti_name_length_rejections = 0;
    std::size_t rtti_name_raw_length_rejections = 0;
    std::size_t rtti_name_output_length_rejections = 0;
    std::size_t rtti_name_failures = 0;
    std::size_t rtti_name_budget_exhausted = 0;
    std::size_t rtti_name_collisions = 0;
    std::size_t rtti_name_collision_roots = 0;
    std::size_t thunk_candidates = 0;
    std::size_t thunk_resolved = 0;
    std::size_t sampled_functions = 0;
    std::size_t decoded_instructions = 0;
    std::size_t string_candidates = 0;
    std::size_t shared_strings = 0;
    std::size_t string_labels = 0;
    std::size_t string_reference_candidates = 0;
    std::size_t string_reference_potential_hits = 0;
    std::size_t string_reference_exact_hits = 0;
    std::size_t string_reference_interior_rejections = 0;
    std::size_t string_reference_ambiguities = 0;
    std::size_t string_reference_shared = 0;
    std::size_t string_reference_terminal_hits_skipped = 0;
    std::size_t string_reference_unindexed = 0;
    std::size_t string_reference_unreachable = 0;
    std::size_t string_reference_overlaps = 0;
    std::size_t string_validation_functions = 0;
    std::size_t string_function_byte_budget_exhausted = 0;
    std::size_t string_function_instruction_budget_exhausted = 0;
    std::size_t string_validation_budget_exhausted = 0;
    std::size_t string_instruction_budget_exhausted = 0;
    std::size_t string_scan_byte_budget_exhausted = 0;
    std::size_t vtable_interior_target_rejections = 0;
    std::size_t thunk_interior_destination_rejections = 0;
    std::size_t approximate_bytes = 0;
};

// Analyzer for one mapped PE64 image; addresses are relative to load_address.
class Engine {
public:
    Engine(const std::uint8_t *image, std::size_t mapped_size, std::uint64_t load_address);
    ~Engine();
    Engine(Engine &&) noexcept;
    Engine &operator=(Engine &&) noexcept;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    bool valid() const;
    const FunctionRange *functionContaining(std::uint64_t rva) const;
    std::unordered_map<std::uint64_t, TypedLabel> guess(std::span<const std::uint64_t> rvas);
    BuildStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Pure ranking helpers kept in the internal Windows component so formal tests
// can lock down ambiguity and string-quality policy without private BDS data.
TypedLabel chooseVtableLabel(std::vector<VtableEvidence> evidence);
int scoreStringHint(std::string_view value);
TypedLabel formatStringHint(std::string_view value);

std::unordered_map<std::uint64_t, TypedLabel> guessCurrentModuleSymbols(std::span<const std::uint64_t> rvas);
BuildStats currentModuleStats();

}  // namespace spark::symbol_guess::windows

#endif

#endif  // ENDSTONE_SPARK_SYMBOL_GUESS_WINDOWS_H
