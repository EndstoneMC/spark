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

// Best-effort readable label for an RVA inside the current process's main
// executable, recovered at runtime without any symbol file. Function extents
// come from platform unwind metadata; names are guessed from validated RTTI
// vtables and, failing that, from decoded semantic-string references. Labels
// include the evidence source (`vtable:`, `str:`); useful but incomplete
// evidence uses `type?:`. Returns an empty string for conflicting or unsafe
// evidence. The underlying index is built lazily during export and cached.
std::string guessMainModuleSymbol(std::uint64_t rva);

// Resolves one export batch at once. Both native backends use the batch to
// decode only sampled function roots and verify candidate string references
// globally; callers should prefer this over repeated single-RVA queries.
std::unordered_map<std::uint64_t, std::string>
guessMainModuleSymbols(std::span<const std::uint64_t> rvas);

// Detailed export-time result used by symbolication. A result with an empty
// label still carries a verified function start so unresolved sampled PCs can
// be normalized to one stable RVA. Inputs outside validated function extents
// are omitted.
std::unordered_map<std::uint64_t, GuessResult>
analyzeMainModuleSymbols(std::span<const std::uint64_t> rvas);

} // namespace spark

#endif // ENDSTONE_SPARK_SYMBOL_GUESS_H
