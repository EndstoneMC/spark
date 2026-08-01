#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_H

#include <cstdint>
#include <string>

namespace spark {

// Best-effort readable label for an RVA inside the current process's main
// executable, recovered at runtime without any symbol file. Function extents come
// from the x64 exception directory (.pdata); names are guessed from RTTI vtable
// class names ("ServerLevel::vfn[3]") and, failing that, from a string literal
// referenced only by that function ("\"...\""). Returns an empty string when
// nothing is known. Windows-only — other platforms always return empty. The
// underlying table is built lazily once per process and cached.
std::string guessMainModuleSymbol(std::uint64_t rva);

}  // namespace spark

#endif  // ENDSTONE_SPARK_SYMBOL_GUESS_H
