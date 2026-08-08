#include "core/util/format.h"

#include <cstdio>

namespace spark {

std::string formatDuration(std::int64_t seconds)
{
    if (seconds < 60) {
        return std::to_string(seconds) + "s";
    }
    std::int64_t m = seconds / 60;
    std::int64_t s = seconds % 60;
    if (m < 60) {
        return std::to_string(m) + "m " + std::to_string(s) + "s";
    }
    std::int64_t h = m / 60;
    m %= 60;
    return std::to_string(h) + "h " + std::to_string(m) + "m";
}

std::string formatBytes(std::uint64_t bytes)
{
    constexpr const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < (sizeof(units) / sizeof(units[0]))) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.2f %s", value, units[unit]);
    return buffer;
}

std::string formatNumber(double value, int precision)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), precision == 1 ? "%.1f" : "%.2f", value);
    return buffer;
}

const std::string &tpsColor(double tps)
{
    if (tps >= 19.5) {
        return kColorGreen;
    }
    if (tps >= 18.0) {
        return kColorYellow;
    }
    if (tps >= 15.0) {
        return kColorGold;
    }
    return kColorRed;
}

const std::string &msptColor(double mspt)
{
    if (mspt <= 25.0) {
        return kColorGreen;
    }
    if (mspt <= 40.0) {
        return kColorYellow;
    }
    if (mspt <= 50.0) {
        return kColorGold;
    }
    return kColorRed;
}

const std::string &cpuColor(double usage)
{
    if (usage < 0.65) {
        return kColorGreen;
    }
    if (usage < 0.85) {
        return kColorYellow;
    }
    return kColorRed;
}

std::string formatTpsValue(const RollingValue &value)
{
    if (!value.present) {
        return kColorGray + std::string("n/a");
    }
    return tpsColor(value.value) + formatNumber(value.value, 1) + kColorGray;
}

std::string formatCpuValue(const RollingValue &value)
{
    if (!value.present) {
        return kColorGray + std::string("n/a");
    }
    return cpuColor(value.value) + formatNumber(value.value * 100.0, 1) + "%" + kColorGray;
}

std::string formatMsptValue(double value)
{
    return msptColor(value) + formatNumber(value, 2) + kColorGray;
}

std::string formatMsptDistribution(const DistributionValues &values)
{
    if (!values.present) {
        return kColorGray + std::string("n/a");
    }
    return formatMsptValue(values.mean) + "/" + formatMsptValue(values.min) + "/" + formatMsptValue(values.median) +
           "/" + formatMsptValue(values.percentile95) + "/" + formatMsptValue(values.max);
}

std::string formatPingRtts(const PingSummary &summary)
{
    if (summary.total() == 0) {
        return kColorGray + std::string("n/a");
    }
    return kColorGreen + std::to_string(summary.min()) + kColorGray + "/" + kColorGreen +
           std::to_string(summary.median()) + kColorGray + "/" + kColorGreen +
           std::to_string(summary.percentile95th()) + kColorGray + "/" + kColorGreen + std::to_string(summary.max()) +
           kColorGray + " ms";
}

std::string formatPingRtts(const PingRollingAverage &average)
{
    if (average.samples() == 0) {
        return kColorGray + std::string("n/a");
    }
    return kColorGreen + std::to_string(average.min()) + kColorGray + "/" + kColorGreen +
           std::to_string(average.median()) + kColorGray + "/" + kColorGreen +
           std::to_string(average.percentile95th()) + kColorGray + "/" + kColorGreen + std::to_string(average.max()) +
           kColorGray + " ms";
}

}  // namespace spark
