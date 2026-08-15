#include "application/placeholder/spark_placeholder_resolver.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "core/util/format.h"

namespace spark {
namespace {

constexpr double KTargetTps = 20.0;
constexpr double KIdealTickDurationMs = 50.0;

std::string decimal(double value, int precision)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), precision == 1 ? "%.1f" : "%.2f", value);
    std::string result(buffer);
    if (precision == 2 && result.ends_with('0')) {
        result.pop_back();
    }
    return result;
}

const std::string &javaTpsColor(double value)
{
    if (value > KTargetTps * 0.9) {
        return kColorGreen;
    }
    if (value > KTargetTps * 0.8) {
        return kColorYellow;
    }
    return kColorRed;
}

const std::string &javaMsptColor(double value)
{
    if (value >= KIdealTickDurationMs) {
        return kColorRed;
    }
    if (value >= KIdealTickDurationMs * 0.8) {
        return kColorYellow;
    }
    return kColorGreen;
}

const std::string &javaCpuColor(double value)
{
    if (value > 0.9) {
        return kColorRed;
    }
    if (value > 0.65) {
        return kColorYellow;
    }
    return kColorGreen;
}

std::optional<std::string> formatTps(const RollingValue &value)
{
    if (!value.present || !std::isfinite(value.value)) {
        return std::nullopt;
    }
    const double rounded = std::round(value.value * 100.0) / 100.0;
    return javaTpsColor(value.value) + (value.value > KTargetTps ? "*" : "") +
           decimal((std::min)(rounded, KTargetTps), 2);
}

std::optional<std::string> formatMspt(const DistributionValues &value)
{
    if (!value.present || !std::isfinite(value.min) || !std::isfinite(value.median) ||
        !std::isfinite(value.percentile95) || !std::isfinite(value.max)) {
        return std::nullopt;
    }
    return javaMsptColor(value.min) + decimal(value.min, 1) + kColorGray + "/" + javaMsptColor(value.median) +
           decimal(value.median, 1) + kColorGray + "/" + javaMsptColor(value.percentile95) +
           decimal(value.percentile95, 1) + kColorGray + "/" + javaMsptColor(value.max) + decimal(value.max, 1);
}

std::optional<std::string> formatCpu(const RollingValue &value)
{
    if (!value.present || !std::isfinite(value.value)) {
        return std::nullopt;
    }
    return javaCpuColor(value.value) + std::to_string(static_cast<int>(value.value * 100.0)) + "%";
}

template <typename Formatter, typename Value>
std::optional<std::string> joinValues(std::initializer_list<const Value *> values, std::string_view separator,
                                      Formatter formatter)
{
    std::string result;
    bool first = true;
    for (const Value *value : values) {
        auto formatted = formatter(*value);
        if (!formatted) {
            return std::nullopt;
        }
        if (!first) {
            result += kColorReset;
            result += separator;
        }
        result += *formatted;
        first = false;
    }
    return result;
}

}  // namespace

std::optional<std::string> resolveSparkPlaceholder(const SparkPlaceholderStatistics &statistics,
                                                   std::string_view params)
{
    if (params == "tps") {
        const RollingValue last_5s = statistics.placeholderTps(5 * 1000);
        const RollingValue last_10s = statistics.placeholderTps(10 * 1000);
        const RollingValue last_1m = statistics.placeholderTps(60 * 1000);
        const RollingValue last_5m = statistics.placeholderTps(5 * 60 * 1000);
        const RollingValue last_15m = statistics.placeholderTps(15 * 60 * 1000);
        return joinValues<decltype(&formatTps), RollingValue>({&last_5s, &last_10s, &last_1m, &last_5m, &last_15m},
                                                              ", ", formatTps);
    }
    if (params == "tps_5s") {
        return formatTps(statistics.placeholderTps(5 * 1000));
    }
    if (params == "tps_10s") {
        return formatTps(statistics.placeholderTps(10 * 1000));
    }
    if (params == "tps_1m") {
        return formatTps(statistics.placeholderTps(60 * 1000));
    }
    if (params == "tps_5m") {
        return formatTps(statistics.placeholderTps(5 * 60 * 1000));
    }
    if (params == "tps_15m") {
        return formatTps(statistics.placeholderTps(15 * 60 * 1000));
    }
    if (params == "tickduration") {
        const DistributionValues last_10s =
            statistics.placeholderTickDuration(StatisticsService::kPlaceholderTickDuration10sSamples);
        const DistributionValues last_1m =
            statistics.placeholderTickDuration(StatisticsService::kPlaceholderTickDuration1mSamples);
        return joinValues<decltype(&formatMspt), DistributionValues>({&last_10s, &last_1m}, ";  ", formatMspt);
    }
    if (params == "tickduration_10s") {
        return formatMspt(statistics.placeholderTickDuration(StatisticsService::kPlaceholderTickDuration10sSamples));
    }
    if (params == "tickduration_1m") {
        return formatMspt(statistics.placeholderTickDuration(StatisticsService::kPlaceholderTickDuration1mSamples));
    }
    if (params == "cpu_system") {
        const RollingValue last_10s = statistics.placeholderCpu(10 * 1000, false);
        const RollingValue last_1m = statistics.placeholderCpu(60 * 1000, false);
        const RollingValue last_15m = statistics.placeholderCpu(15 * 60 * 1000, false);
        return joinValues<decltype(&formatCpu), RollingValue>({&last_10s, &last_1m, &last_15m}, ", ", formatCpu);
    }
    if (params == "cpu_system_10s") {
        return formatCpu(statistics.placeholderCpu(10 * 1000, false));
    }
    if (params == "cpu_system_1m") {
        return formatCpu(statistics.placeholderCpu(60 * 1000, false));
    }
    if (params == "cpu_system_15m") {
        return formatCpu(statistics.placeholderCpu(15 * 60 * 1000, false));
    }
    if (params == "cpu_process") {
        const RollingValue last_10s = statistics.placeholderCpu(10 * 1000, true);
        const RollingValue last_1m = statistics.placeholderCpu(60 * 1000, true);
        const RollingValue last_15m = statistics.placeholderCpu(15 * 60 * 1000, true);
        return joinValues<decltype(&formatCpu), RollingValue>({&last_10s, &last_1m, &last_15m}, ", ", formatCpu);
    }
    if (params == "cpu_process_10s") {
        return formatCpu(statistics.placeholderCpu(10 * 1000, true));
    }
    if (params == "cpu_process_1m") {
        return formatCpu(statistics.placeholderCpu(60 * 1000, true));
    }
    if (params == "cpu_process_15m") {
        return formatCpu(statistics.placeholderCpu(15 * 60 * 1000, true));
    }
    return std::nullopt;
}

}  // namespace spark
