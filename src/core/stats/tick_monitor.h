#ifndef ENDSTONE_SPARK_TICK_MONITOR_H
#define ENDSTONE_SPARK_TICK_MONITOR_H

#include <cstddef>
#include <cstdint>

namespace spark {

enum class TickMonitorMode {
    Percentage,
    Duration,
};

struct TickMonitorConfig {
    TickMonitorMode mode = TickMonitorMode::Percentage;
    double threshold = 100.0;
    std::size_t setup_ticks = 120;
};

struct TickMonitorUpdate {
    bool setup_completed = false;
    bool report = false;
    std::uint64_t tick = 0;
    double duration_ms = 0.0;
    double baseline_ms = 0.0;
    double percentage_change = 0.0;
    double setup_min_ms = 0.0;
    double setup_max_ms = 0.0;
};

// Main-thread state machine used by /spark tickmonitor. The first setup_ticks
// observations establish a fixed baseline; subsequent ticks are compared with
// either that baseline or an absolute duration threshold.
class TickMonitor {
public:
    bool start(const TickMonitorConfig &config);
    void stop();

    bool running() const
    {
        return running_;
    }

    const TickMonitorConfig &config() const
    {
        return config_;
    }

    TickMonitorUpdate onTick(double duration_ms);

private:
    TickMonitorConfig config_{};
    bool running_ = false;
    bool monitoring_ = false;
    std::uint64_t ticks_ = 0;
    std::size_t setup_count_ = 0;
    double setup_sum_ms_ = 0.0;
    double setup_min_ms_ = 0.0;
    double setup_max_ms_ = 0.0;
    double baseline_ms_ = 0.0;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_TICK_MONITOR_H
