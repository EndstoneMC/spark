#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "application/placeholder/spark_placeholder_resolver.h"
#include "core/util/format.h"

namespace {

spark::RollingValue rolling(double value)
{
    spark::RollingValue result;
    result.present = true;
    result.value = value;
    return result;
}

spark::DistributionValues distribution(double min, double median, double percentile95, double max)
{
    spark::DistributionValues result;
    result.present = true;
    result.min = min;
    result.median = median;
    result.percentile95 = percentile95;
    result.max = max;
    return result;
}

class FakeStatistics final : public spark::SparkPlaceholderStatistics {
public:
    [[nodiscard]] spark::RollingValue placeholderTps(std::int64_t window_ms) const override
    {
        ++tps_calls;
        tps_windows.push_back(window_ms);
        if (window_ms == 5 * 1000) {
            return rolling(20.4);
        }
        if (window_ms == 10 * 1000) {
            return rolling(19.956);
        }
        if (window_ms == 60 * 1000) {
            return rolling(17.0);
        }
        if (window_ms == 5 * 60 * 1000) {
            return rolling(16.0);
        }
        if (window_ms == 15 * 60 * 1000) {
            return rolling(10.0);
        }
        return {};
    }

    [[nodiscard]] spark::DistributionValues placeholderTickDuration(std::size_t max_samples) const override
    {
        ++duration_calls;
        duration_samples.push_back(max_samples);
        if (max_samples == 200) {
            return distribution(1.04, 39.96, 49.99, 50.0);
        }
        if (max_samples == 1200) {
            return distribution(2.0, 25.0, 40.0, 75.0);
        }
        return {};
    }

    [[nodiscard]] spark::RollingValue placeholderCpu(std::int64_t window_ms, bool process) const override
    {
        ++cpu_calls;
        cpu_windows.push_back(window_ms);
        cpu_process.push_back(process);
        if (!process && window_ms == 10 * 1000) {
            return rolling(0.65);
        }
        if (!process && window_ms == 60 * 1000) {
            return rolling(0.651);
        }
        if (!process && window_ms == 15 * 60 * 1000) {
            return rolling(0.91);
        }
        if (process && window_ms == 10 * 1000) {
            return rolling(0.1);
        }
        if (process && window_ms == 60 * 1000) {
            return rolling(0.7);
        }
        if (process && window_ms == 15 * 60 * 1000) {
            return rolling(1.0);
        }
        return {};
    }

    mutable int tps_calls = 0;
    mutable int duration_calls = 0;
    mutable int cpu_calls = 0;
    mutable std::vector<std::int64_t> tps_windows;
    mutable std::vector<std::size_t> duration_samples;
    mutable std::vector<std::int64_t> cpu_windows;
    mutable std::vector<bool> cpu_process;
};

class UnavailableStatistics final : public spark::SparkPlaceholderStatistics {
public:
    [[nodiscard]] spark::RollingValue placeholderTps(std::int64_t) const override { return {}; }
    [[nodiscard]] spark::DistributionValues placeholderTickDuration(std::size_t) const override { return {}; }
    [[nodiscard]] spark::RollingValue placeholderCpu(std::int64_t, bool) const override { return {}; }
};

class ServiceStatistics final : public spark::SparkPlaceholderStatistics {
public:
    explicit ServiceStatistics(const spark::StatisticsService &statistics) : statistics_(statistics) {}

    [[nodiscard]] spark::RollingValue placeholderTps(std::int64_t window_ms) const override
    {
        return statistics_.placeholderTps(window_ms);
    }

    [[nodiscard]] spark::DistributionValues placeholderTickDuration(std::size_t max_samples) const override
    {
        return statistics_.placeholderTickDuration(max_samples);
    }

    [[nodiscard]] spark::RollingValue placeholderCpu(std::int64_t window_ms, bool process) const override
    {
        return statistics_.placeholderCpu(window_ms, process);
    }

private:
    const spark::StatisticsService &statistics_;
};

void requireValue(const spark::SparkPlaceholderStatistics &statistics, std::string_view params,
                  const std::string &expected)
{
    const auto actual = spark::resolveSparkPlaceholder(statistics, params);
    assert(actual.has_value());
    assert(*actual == expected);
}

}  // namespace

int main()
{
    FakeStatistics statistics;

    requireValue(statistics, "tps_5s", spark::kColorGreen + "*20.0");
    requireValue(statistics, "tps_10s", spark::kColorGreen + "19.96");
    requireValue(statistics, "tps_1m", spark::kColorYellow + "17.0");
    requireValue(statistics, "tps_5m", spark::kColorRed + "16.0");
    requireValue(statistics, "tps_15m", spark::kColorRed + "10.0");
    requireValue(statistics, "tps",
                 spark::kColorGreen + "*20.0" + spark::kColorReset + ", " + spark::kColorGreen + "19.96" +
                     spark::kColorReset + ", " + spark::kColorYellow + "17.0" + spark::kColorReset + ", " +
                     spark::kColorRed + "16.0" + spark::kColorReset + ", " + spark::kColorRed + "10.0");

    const std::string mspt_10s = spark::kColorGreen + "1.0" + spark::kColorGray + "/" + spark::kColorGreen + "40.0" +
                                 spark::kColorGray + "/" + spark::kColorYellow + "50.0" + spark::kColorGray + "/" +
                                 spark::kColorRed + "50.0";
    const std::string mspt_1m = spark::kColorGreen + "2.0" + spark::kColorGray + "/" + spark::kColorGreen + "25.0" +
                                spark::kColorGray + "/" + spark::kColorYellow + "40.0" + spark::kColorGray + "/" +
                                spark::kColorRed + "75.0";
    requireValue(statistics, "tickduration_10s", mspt_10s);
    requireValue(statistics, "tickduration_1m", mspt_1m);
    requireValue(statistics, "tickduration", mspt_10s + spark::kColorReset + ";  " + mspt_1m);

    requireValue(statistics, "cpu_system_10s", spark::kColorGreen + "65%");
    requireValue(statistics, "cpu_system_1m", spark::kColorYellow + "65%");
    requireValue(statistics, "cpu_system_15m", spark::kColorRed + "91%");
    requireValue(statistics, "cpu_system",
                 spark::kColorGreen + "65%" + spark::kColorReset + ", " + spark::kColorYellow + "65%" +
                     spark::kColorReset + ", " + spark::kColorRed + "91%");
    requireValue(statistics, "cpu_process_10s", spark::kColorGreen + "10%");
    requireValue(statistics, "cpu_process_1m", spark::kColorYellow + "70%");
    requireValue(statistics, "cpu_process_15m", spark::kColorRed + "100%");
    requireValue(statistics, "cpu_process",
                 spark::kColorGreen + "10%" + spark::kColorReset + ", " + spark::kColorYellow + "70%" +
                     spark::kColorReset + ", " + spark::kColorRed + "100%");

    const std::vector<std::string_view> supported = {
        "tps",
        "tps_5s",
        "tps_10s",
        "tps_1m",
        "tps_5m",
        "tps_15m",
        "tickduration",
        "tickduration_10s",
        "tickduration_1m",
        "cpu_system",
        "cpu_system_10s",
        "cpu_system_1m",
        "cpu_system_15m",
        "cpu_process",
        "cpu_process_10s",
        "cpu_process_1m",
        "cpu_process_15m",
    };
    const UnavailableStatistics unavailable;
    for (std::string_view params : supported) {
        assert(!spark::resolveSparkPlaceholder(unavailable, params).has_value());
    }

    const int tps_before = statistics.tps_calls;
    const int duration_before = statistics.duration_calls;
    const int cpu_before = statistics.cpu_calls;
    requireValue(statistics, "tps_5s", spark::kColorGreen + "*20.0");
    assert(statistics.tps_calls == tps_before + 1);
    assert(statistics.duration_calls == duration_before);
    assert(statistics.cpu_calls == cpu_before);
    requireValue(statistics, "tickduration_10s", mspt_10s);
    assert(statistics.tps_calls == tps_before + 1);
    assert(statistics.duration_calls == duration_before + 1);
    assert(statistics.cpu_calls == cpu_before);
    requireValue(statistics, "cpu_process_10s", spark::kColorGreen + "10%");
    assert(statistics.tps_calls == tps_before + 1);
    assert(statistics.duration_calls == duration_before + 1);
    assert(statistics.cpu_calls == cpu_before + 1);

    spark::StatisticsService duration_statistics;
    duration_statistics.startAt(0, 0, {});
    duration_statistics.recordTickAt(39.9, 1);
    duration_statistics.recordTickAt(40.0, 2);
    duration_statistics.recordTickAt(49.9, 3);
    duration_statistics.recordTickAt(50.0, 4);
    const ServiceStatistics duration_service(duration_statistics);
    const std::string java_duration = spark::kColorGreen + "39.9" + spark::kColorGray + "/" + spark::kColorYellow +
                                      "49.9" + spark::kColorGray + "/" + spark::kColorRed + "50.0" + spark::kColorGray +
                                      "/" + spark::kColorRed + "50.0";
    requireValue(duration_service, "tickduration_10s", java_duration);
    requireValue(duration_service, "tickduration_1m", java_duration);
    requireValue(duration_service, "tickduration", java_duration + spark::kColorReset + ";  " + java_duration);

    const int calls_before_unknown = statistics.tps_calls + statistics.duration_calls + statistics.cpu_calls;
    assert(!spark::resolveSparkPlaceholder(statistics, "TPS").has_value());
    assert(!spark::resolveSparkPlaceholder(statistics, "tps_5s_extra").has_value());
    assert(!spark::resolveSparkPlaceholder(statistics, "tickduration_5m").has_value());
    assert(!spark::resolveSparkPlaceholder(statistics, "cpu_player_10s").has_value());
    assert(!spark::resolveSparkPlaceholder(statistics, "unknown").has_value());
    assert(!spark::resolveSparkPlaceholder(statistics, "").has_value());
    assert(statistics.tps_calls + statistics.duration_calls + statistics.cpu_calls == calls_before_unknown);
}
