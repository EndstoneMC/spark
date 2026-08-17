#ifndef ENDSTONE_SPARK_PROFILING_WINDOW_H
#define ENDSTONE_SPARK_PROFILING_WINDOW_H

#include <cstdint>

namespace spark::profiling_window {

// Match upstream spark's profiler/viewer time-window semantics:
// one profiling window is one minute, and continuous profiling retains
// 60 historical windows (approximately one hour).
inline constexpr std::int32_t kSizeSeconds = 60;
inline constexpr std::int64_t kSizeMs = static_cast<std::int64_t>(kSizeSeconds) * 1000;
inline constexpr std::int32_t kHistorySize = 60;
inline constexpr std::int64_t kHistoryMs = static_cast<std::int64_t>(kHistorySize) * kSizeMs;

}  // namespace spark::profiling_window

#endif  // ENDSTONE_SPARK_PROFILING_WINDOW_H
