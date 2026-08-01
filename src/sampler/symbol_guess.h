#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_H

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

namespace spark {

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

} // namespace spark

#endif // ENDSTONE_SPARK_SYMBOL_GUESS_H
