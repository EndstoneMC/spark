#ifndef ENDSTONE_SPARK_CONSTANTS_H
#define ENDSTONE_SPARK_CONSTANTS_H

namespace spark {

inline constexpr const char *kVersion = "0.6.0";

inline constexpr int kSparkFormatVersion = 2;

// spark's public infrastructure — the profile uploads here and opens in the real viewer.
inline constexpr const char *kBytebinUrl = "https://spark-usercontent.lucko.me/";
inline constexpr const char *kViewerUrl = "https://spark.lucko.me/";
inline constexpr const char *kSamplerContentType = "application/x-spark-sampler";
inline constexpr const char *kHealthContentType = "application/x-spark-health";

}  // namespace spark

#endif  // ENDSTONE_SPARK_CONSTANTS_H
