#ifndef ENDSTONE_SPARK_SYMBOLICATE_H
#define ENDSTONE_SPARK_SYMBOLICATE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "native/symbol/symbol_guess.h"
#include "native/sampler/types.h"

namespace spark {

// A frame after symbol resolution, in spark's className/methodName terms.
struct ResolvedFrame {
  std::string class_name;  // module basename, e.g. "bedrock_server"
  std::string method_name; // demangled symbol, or "0x<rva>" when stripped
  std::string method_desc; // optional descriptor (unused for now)
  std::int32_t line = -1;  // source line if DWARF is present, else -1
  // Non-zero only when unwind metadata validated a native function root for
  // an otherwise unresolved main-module frame. The serializer records this
  // separately from the sampled PC so offline evaluation can measure range
  // coverage without fragmenting the viewer label.
  std::uint64_t guessed_function_rva = 0;
};

// Small deterministic policy surface used by the platform backends and formal
// tests. A guess is appended only when the frame is proven to belong to the
// executable and no normal symbol backend supplied a method name.
bool frameMatchesMainModule(std::uint64_t raw_address, std::uint64_t rva,
                            std::uint64_t module_base,
                            std::uint64_t module_size);
void applySymbolGuessFallback(ResolvedFrame &frame, std::uint64_t rva,
                              bool main_module, std::string_view guess);
void applySymbolGuessFallback(ResolvedFrame &frame, std::uint64_t rva,
                              bool main_module, const GuessResult &guess);

// Resolve a batch of unique frame keys via the platform symbol backend. Frames
// whose symbol cannot be recovered fall back to "0x<rva>" — expected for
// stripped binaries and offline-symbolicatable later against IDA/PDB. On
// Windows and Linux, unresolved frames in the main executable additionally
// carry a parenthesized, evidence-tagged runtime guess when one is available.
// Normal PDB/dynamic symbols always replace the RVA and are never annotated as
// guesses.
std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash>
resolveFrames(const ModuleTable &modules, const std::vector<FrameKey> &keys);

// True if the given runtime address resolves to a sleep/wait function
// (nanosleep, futex, poll, …). Used to drop idle execution samples.
bool isSleepFrame(std::uint64_t raw_address);

} // namespace spark

#endif // ENDSTONE_SPARK_SYMBOLICATE_H
