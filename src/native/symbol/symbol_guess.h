#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_H

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

namespace spark {

enum class GuessKind {
    None,
    Rtti,
    String,
    Vtable,
    Thunk,
    Call,
    Import,
    Context,
    Type,
};

enum class Confidence {
    None,
    Low,
    Medium,
    High,
};

struct GuessResult {
    std::string label;
    GuessKind kind = GuessKind::None;
    Confidence confidence = Confidence::None;
    std::uint32_t evidence_count = 0;
    std::uint64_t function_rva = 0;
};

// Best-effort runtime label for a main-module RVA without symbol files. Names come from
// RTTI vtables and semantic-string references; labels include evidence source (`vtable:`, `str:`, `type?:`).
std::string guessMainModuleSymbol(std::uint64_t rva);

// Batch resolution for sampled function roots; prefer over repeated single-RVA queries.
std::unordered_map<std::uint64_t, std::string> guessMainModuleSymbols(std::span<const std::uint64_t> rvas);

// Detailed export-time result. Even empty-label results carry a verified function start
// for RVA normalization. Inputs outside validated function extents are omitted.
std::unordered_map<std::uint64_t, GuessResult> analyzeMainModuleSymbols(std::span<const std::uint64_t> rvas);

}  // namespace spark

#endif  // ENDSTONE_SPARK_SYMBOL_GUESS_H
