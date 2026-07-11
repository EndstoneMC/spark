#ifndef ENDSTONE_SPARK_SYMBOLICATE_H
#define ENDSTONE_SPARK_SYMBOLICATE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "sampler/types.h"

namespace spark {

// A frame after symbol resolution, in spark's className/methodName terms.
struct ResolvedFrame {
    std::string class_name;   // module basename, e.g. "bedrock_server"
    std::string method_name;  // demangled symbol, or "0x<rva>" when stripped
    std::string method_desc;  // optional descriptor (unused for now)
    std::int32_t line = -1;   // source line if DWARF is present, else -1
};

// Resolve a batch of unique frame keys via cpptrace (reads DWARF/symbols off disk).
// Frames whose symbol cannot be recovered fall back to "0x<rva>" — expected for the
// stripped BDS binary and offline-symbolicatable later against IDA/PDB.
std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> resolveFrames(const ModuleTable &modules,
                                                                        const std::vector<FrameKey> &keys);

// True if the given runtime address resolves to a sleep/wait function (nanosleep,
// futex, poll, …). Used to drop the server thread's inter-tick idle samples.
bool isSleepFrame(std::uint64_t raw_address);

}  // namespace spark

#endif  // ENDSTONE_SPARK_SYMBOLICATE_H
