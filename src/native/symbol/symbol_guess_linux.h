#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_LINUX_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_LINUX_H

#if defined(__linux__) && defined(__x86_64__)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "native/symbol/symbol_guess_dwarf.h"
#include "native/symbol/symbol_guess_evidence.h"

namespace spark::symbol_guess::linux {

inline constexpr std::size_t kMaximumFunctionDecodeBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumFunctionDecodeInstructions = 8192U;
inline constexpr std::size_t kMaximumBatchFunctionValidations = 4096U;
inline constexpr std::size_t kMaximumBatchDecodedInstructions = 1000000U;

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
    std::size_t vtable_interior_target_rejections = 0;
    std::size_t thunk_interior_destination_rejections = 0;
    std::size_t sampled_functions = 0;
    std::size_t decoded_instructions = 0;
    std::size_t string_candidates = 0;
    std::size_t shared_strings = 0;
    std::size_t string_labels = 0;
    std::size_t string_accumulated_labels = 0;
    std::size_t lambda_body_labels = 0;
    std::size_t code_pattern_labels = 0;
    std::size_t thunk_candidates = 0;
    std::size_t thunk_resolved = 0;
    std::size_t thunk_labels = 0;
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
    std::size_t rtti_types = 0;
    std::size_t rtti_bases = 0;
    std::size_t vtable_inheritance_resolved = 0;
    std::size_t approximate_bytes = 0;
};

struct DecodedThunk {
    // Direct jumps store the destination. Indirect jumps store the address of
    // the pointer slot, which the caller must read through a bounded image view.
    std::uint64_t target = 0;
    bool indirect = false;
    bool adjusts_this = false;
};

// Decode reachable x86-64 instructions in one function extent and return the
// targets of RIP-relative LEA instructions. Exposed so public synthetic tests
// can prove that opcode-like bytes inside another instruction are ignored.
std::vector<std::uint64_t> decodeRipRelativeLeaTargets(std::span<const std::uint8_t> code, std::uint64_t function_rva,
                                                       std::size_t *decoded_instructions = nullptr);

// Accept only a direct/RIP-indirect jump, optionally preceded by one proven
// this adjustment or one RIP-relative load into the jump register.
std::optional<DecodedThunk> decodeStrictThunk(std::span<const std::uint8_t> code, std::uint64_t function_rva,
                                              std::size_t *decoded_instructions = nullptr);

// Follow at most max_depth validated thunk edges. Cycles and longer chains are
// rejected instead of returning a misleading intermediate target.
std::optional<std::uint64_t> followStrictThunkChain(
    std::uint64_t start, const std::function<std::optional<std::uint64_t>(std::uint64_t)> &next,
    std::size_t max_depth = 2);

BuildStats currentModuleStats();

TypedLabel decodeCodePattern(std::span<const std::uint8_t> code, std::uint64_t function_rva,
                             std::size_t *decoded_instructions = nullptr, bool reverse_worklist = false);

struct LambdaWrapper {
    std::uint64_t root = 0;
    std::string owner;
    std::vector<std::uint64_t> targets;
};

struct LambdaBodyIndex {
    bool complete = false;
    std::vector<LambdaWrapper> wrappers;
    std::unordered_map<std::uint64_t, TypedLabel> labels;
};

LambdaBodyIndex collectLambdaBodyIndex(const dwarf::ImageView &image, const std::vector<dwarf::FunctionRange> &ranges,
                                       const std::unordered_map<std::uint64_t, TypedLabel> &labels,
                                       std::size_t *decoded_instructions = nullptr);
TypedLabel projectLambdaBodyLabel(const LambdaBodyIndex &index, std::uint64_t root, const TypedLabel &earlier = {});

}  // namespace spark::symbol_guess::linux

#endif

#endif  // ENDSTONE_SPARK_SYMBOL_GUESS_LINUX_H
