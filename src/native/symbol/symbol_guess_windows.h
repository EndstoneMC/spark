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
    std::size_t thunk_candidates = 0;
    std::size_t thunk_resolved = 0;
    std::size_t sampled_functions = 0;
    std::size_t decoded_instructions = 0;
    std::size_t string_candidates = 0;
    std::size_t shared_strings = 0;
    std::size_t string_labels = 0;
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
