#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_H

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

namespace spark {

// Best-effort readable label for an RVA inside the current process's main
// executable, recovered at runtime without any symbol file. Function extents
// come from the x64 exception directory (.pdata); names are guessed from RTTI
// vtable class names ("ServerLevel::vfn[3]") and, failing that, from a string
// literal referenced only by that function ("\"...\""). Returns an empty string
// when nothing is known. Windows and Linux use platform unwind/RTTI metadata;
// other platforms return empty. The underlying index is built lazily and
// cached.
std::string guessMainModuleSymbol(std::uint64_t rva);

// Resolves one export batch at once. Windows uses the batch to decode only the
// sampled function roots and to verify candidate string references globally;
// callers should prefer this over repeated single-RVA queries.
std::unordered_map<std::uint64_t, std::string> guessMainModuleSymbols(std::span<const std::uint64_t> rvas);

} // namespace spark

#endif // ENDSTONE_SPARK_SYMBOL_GUESS_H
