#ifndef ENDSTONE_SPARK_EXECUTABLE_HASH_H
#define ENDSTONE_SPARK_EXECUTABLE_HASH_H

#include <string>
#include <string_view>

namespace spark {

// Returns a lowercase SHA-256 digest.
std::string sha256Hex(std::string_view bytes);

// Hashes the executable backing the current process. On Linux uses /proc/self/exe
// so the path cannot be swapped at runtime. Returns empty string on failure.
std::string currentExecutableSha256(std::string &error);

}  // namespace spark

#endif  // ENDSTONE_SPARK_EXECUTABLE_HASH_H
