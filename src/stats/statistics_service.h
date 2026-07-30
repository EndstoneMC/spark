#ifndef ENDSTONE_SPARK_STATISTICS_SERVICE_H
#define ENDSTONE_SPARK_STATISTICS_SERVICE_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "stats/system_stats.h"

namespace spark {

struct RollingValue {
    bool present = false;
    double value = 0.0;
    std::int64_t span_ms = 0;
    std::size_t samples = 0;
};

struct DistributionValues {
    bool present = false;
    double mean = 0.0;
    double min = 0.0;
    double median = 0.0;
    double percentile95 = 0.0;
    double max = 0.0;
    std::int64_t span_ms = 0;
    std::size_t samples = 0;
};

struct TpsStatistics {
    RollingValue last_5s;
    RollingValue last_10s;
    RollingValue last_1m;
    RollingValue last_5m;
    RollingValue last_15m;
};

struct MsptStatistics {
    DistributionValues last_10s;
    DistributionValues last_1m;
    DistributionValues last_5m;
};

struct CpuRollingStatistics {
    RollingValue process_last_10s;
    RollingValue process_last_1m;
    RollingValue process_last_15m;
    RollingValue system_last_10s;
    RollingValue system_last_1m;
    RollingValue system_last_15m;
};

struct StatisticsSnapshot {
    std::int64_t generated_time_ms = 0;
    std::int64_t history_span_ms = 0;
    TpsStatistics tps;
    MsptStatistics mspt;
    CpuRollingStatistics cpu;
};

// Maintains a fixed-capacity, profiler-independent history of completed ticks
// and one-second CPU observations. The per-tick path performs no allocation or
// sorting; percentile work is deferred until snapshot() is requested.
class StatisticsService {
public:
    static constexpr std::int64_t kMaximumHistoryMs = 15 * 60 * 1000;
    static constexpr std::size_t kTickCapacity = 15 * 60 * 20;
    static constexpr std::size_t kCpuCapacity = 15 * 60;

    void start();

    // Records one completed server tick. Returns true approximately once per
    // second so the main-thread owner can refresh inexpensive server gauges.
    bool onTick(double duration_ms);

    StatisticsSnapshot snapshot() const;

    // Deterministic clock/CPU entry points used by the offline self-test.
    void startAt(std::int64_t steady_ms, std::int64_t unix_ms,
                 const CpuSnapshot &initial_cpu);
    void recordTickAt(double duration_ms, std::int64_t steady_ms);
    void recordCpuSnapshot(const CpuSnapshot &current);
    StatisticsSnapshot snapshotAt(std::int64_t steady_ms) const;

    std::int64_t unixTimeFor(std::int64_t steady_ms) const;
    std::int64_t lastObservationSteadyMs() const
    {
        return last_observation_steady_ms_;
    }

private:
    struct TickSample {
        std::int64_t steady_ms = 0;
        double duration_ms = 0.0;
        bool duration_valid = false;
    };

    struct CpuSample {
        std::int64_t start_steady_ms = 0;
        std::int64_t end_steady_ms = 0;
        double process = 0.0;
        double system = 0.0;
        bool process_valid = false;
        bool system_valid = false;
    };

    RollingValue tpsFor(std::int64_t now_ms, std::int64_t window_ms) const;
    DistributionValues msptFor(std::int64_t now_ms,
                               std::int64_t window_ms) const;
    RollingValue cpuFor(std::int64_t now_ms, std::int64_t window_ms,
                        bool process) const;
    std::int64_t effectiveStart(std::int64_t now_ms,
                                std::int64_t window_ms) const;

    std::array<TickSample, kTickCapacity> ticks_{};
    std::array<CpuSample, kCpuCapacity> cpu_{};
    std::size_t tick_begin_ = 0;
    std::size_t tick_size_ = 0;
    std::size_t cpu_begin_ = 0;
    std::size_t cpu_size_ = 0;
    std::int64_t start_steady_ms_ = 0;
    std::int64_t start_unix_ms_ = 0;
    std::int64_t last_observation_steady_ms_ = 0;
    std::int64_t next_cpu_sample_steady_ms_ = 0;
    CpuSnapshot previous_cpu_{};
    bool started_ = false;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_STATISTICS_SERVICE_H
