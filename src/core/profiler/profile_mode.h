#ifndef ENDSTONE_SPARK_PROFILE_MODE_H
#define ENDSTONE_SPARK_PROFILE_MODE_H

#include <cstdint>

namespace spark {

enum class ProfileMode : std::uint8_t {
    Execution,
    Allocation,
};

// Matches upstream spark's ThreadGrouper proto enum.
enum class ThreadGrouperMode : std::uint8_t {
    ByName = 0,   // each thread gets its own tree
    ByPool = 1,   // threads grouped by pool name
    AsOne = 2,    // all threads merged into one tree
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_PROFILE_MODE_H
