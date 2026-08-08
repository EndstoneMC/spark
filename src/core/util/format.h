#ifndef SPARK_CORE_UTIL_FORMAT_H
#define SPARK_CORE_UTIL_FORMAT_H

#include <cstdint>
#include <string>

#include "core/stats/ping_statistics.h"
#include "core/stats/statistics_service.h"

namespace spark {

// Minecraft chat color codes (protocol-level, not platform-specific).
// Equivalent to endstone::ColorFormat but without the Endstone dependency.
inline const std::string kColorGold = "\xc2\xa7""6";     // §6
inline const std::string kColorGray = "\xc2\xa7""7";     // §7
inline const std::string kColorGreen = "\xc2\xa7""a";    // §a
inline const std::string kColorYellow = "\xc2\xa7""e";   // §e
inline const std::string kColorRed = "\xc2\xa7""c";      // §c
inline const std::string kColorReset = "\xc2\xa7""r";    // §r

std::string formatDuration(std::int64_t seconds);
std::string formatBytes(std::uint64_t bytes);
std::string formatNumber(double value, int precision);

const std::string &tpsColor(double tps);
const std::string &msptColor(double mspt);
const std::string &cpuColor(double usage);

std::string formatTpsValue(const RollingValue &value);
std::string formatCpuValue(const RollingValue &value);
std::string formatMsptValue(double value);
std::string formatMsptDistribution(const DistributionValues &values);
std::string formatPingRtts(const PingSummary &summary);
std::string formatPingRtts(const PingRollingAverage &average);

}  // namespace spark

#endif  // SPARK_CORE_UTIL_FORMAT_H
