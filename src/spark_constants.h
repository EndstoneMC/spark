#ifndef ENDSTONE_SPARK_CONSTANTS_H
#define ENDSTONE_SPARK_CONSTANTS_H

namespace spark {

inline constexpr const char *kVersion = "0.2.0";

// The spark viewer treats a profile as "supported" (no old-version warning, all
// features enabled) when platform_metadata.spark_version >= 2. We emit the modern
// flattened node/time-window format, so advertise a recent spark build number.
inline constexpr int kSparkFormatVersion = 400;

// spark's public infrastructure — the profile uploads here and opens in the real viewer.
inline constexpr const char *kBytebinUrl = "https://spark-usercontent.lucko.me/";
inline constexpr const char *kViewerUrl = "https://spark.lucko.me/";
inline constexpr const char *kSamplerContentType = "application/x-spark-sampler";

}  // namespace spark

#endif  // ENDSTONE_SPARK_CONSTANTS_H
