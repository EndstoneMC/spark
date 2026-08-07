#ifndef ENDSTONE_SPARK_EXECUTABLE_HASH_H
#define ENDSTONE_SPARK_EXECUTABLE_HASH_H

#include <string>
#include <string_view>

namespace spark {

// Returns a lowercase SHA-256 digest. Exposed so the offline self-test can
// validate the implementation against standard vectors.
std::string sha256Hex(std::string_view bytes);

// Hashes the executable image backing the current process. On Linux this opens
// /proc/self/exe so replacing the executable path while the process is running
// cannot make a profile identify the wrong file. Returns an empty string and a
// diagnostic on failure.
std::string currentExecutableSha256(std::string &error);

}  // namespace spark

#endif  // ENDSTONE_SPARK_EXECUTABLE_HASH_H
