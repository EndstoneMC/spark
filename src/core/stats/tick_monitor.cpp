#include "core/stats/tick_monitor.h"

#include <algorithm>
#include <cmath>

namespace spark {

bool TickMonitor::start(const TickMonitorConfig &config)
{
    if (!std::isfinite(config.threshold) || config.threshold <= 0.0 || config.setup_ticks == 0) {
        return false;
    }

    config_ = config;
    running_ = true;
    monitoring_ = false;
    ticks_ = 0;
    setup_count_ = 0;
    setup_sum_ms_ = 0.0;
    setup_min_ms_ = 0.0;
    setup_max_ms_ = 0.0;
    baseline_ms_ = 0.0;
    return true;
}

void TickMonitor::stop()
{
    running_ = false;
    monitoring_ = false;
}

TickMonitorUpdate TickMonitor::onTick(double duration_ms)
{
    TickMonitorUpdate update;
    if (!running_ || !std::isfinite(duration_ms) || duration_ms < 0.0) {
        return update;
    }

    update.tick = ++ticks_;
    update.duration_ms = duration_ms;

    if (!monitoring_) {
        if (setup_count_ == 0) {
            setup_min_ms_ = duration_ms;
            setup_max_ms_ = duration_ms;
        }
        else {
            setup_min_ms_ = std::min(setup_min_ms_, duration_ms);
            setup_max_ms_ = std::max(setup_max_ms_, duration_ms);
        }
        setup_sum_ms_ += duration_ms;
        ++setup_count_;

        if (setup_count_ == config_.setup_ticks) {
            baseline_ms_ = setup_sum_ms_ / static_cast<double>(setup_count_);
            monitoring_ = true;
            update.setup_completed = true;
            update.baseline_ms = baseline_ms_;
            update.setup_min_ms = setup_min_ms_;
            update.setup_max_ms = setup_max_ms_;
        }
        return update;
    }

    update.baseline_ms = baseline_ms_;
    if (baseline_ms_ > 0.0) {
        update.percentage_change = ((duration_ms - baseline_ms_) * 100.0) / baseline_ms_;
    }

    if (duration_ms <= baseline_ms_) {
        return update;
    }

    update.report = config_.mode == TickMonitorMode::Duration
                        ? duration_ms > config_.threshold
                        : update.percentage_change > config_.threshold;
    return update;
}

}  // namespace spark
