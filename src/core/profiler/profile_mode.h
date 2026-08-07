#ifndef ENDSTONE_SPARK_PROFILE_MODE_H
#define ENDSTONE_SPARK_PROFILE_MODE_H

#include <cstdint>

namespace spark {

enum class ProfileMode : std::uint8_t {
    Execution,
    Allocation,
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_PROFILE_MODE_H
