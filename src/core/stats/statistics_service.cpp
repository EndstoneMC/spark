#include "core/stats/statistics_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

namespace spark {
namespace {

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t unixNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double clampUsage(double value)
{
    return (std::max)(0.0, (std::min)(1.0, value));
}

}  // namespace

void StatisticsService::start()
{
    const std::int64_t steady_ms = steadyNowMs();
    CpuSnapshot initial = captureCpuSnapshot();
    startAt(steady_ms, unixNowMs(), initial);
}

void StatisticsService::startAt(std::int64_t steady_ms, std::int64_t unix_ms, const CpuSnapshot &initial_cpu)
{
    tick_begin_ = 0;
    tick_size_ = 0;
    cpu_begin_ = 0;
    cpu_size_ = 0;
    gauge_begin_ = 0;
    gauge_size_ = 0;
    start_steady_ms_ = steady_ms;
    start_unix_ms_ = unix_ms;
    last_observation_steady_ms_ = steady_ms;
    next_cpu_sample_steady_ms_ = steady_ms + 1000;
    previous_cpu_ = initial_cpu;
    started_ = true;
}

bool StatisticsService::onTick(double duration_ms)
{
    if (!started_) {
        start();
    }

    const std::int64_t now_ms = steadyNowMs();
    recordTickAt(duration_ms, now_ms);
    if (now_ms < next_cpu_sample_steady_ms_) {
        return false;
    }

    recordCpuSnapshot(captureCpuSnapshot());
    next_cpu_sample_steady_ms_ = now_ms + 1000;
    return true;
}

void StatisticsService::recordTickAt(double duration_ms, std::int64_t steady_ms)
{
    if (!started_) {
        startAt(steady_ms, steady_ms, CpuSnapshot{});
    }
    if (steady_ms < last_observation_steady_ms_) {
        steady_ms = last_observation_steady_ms_;
    }

    TickSample sample;
    sample.steady_ms = steady_ms;
    sample.duration_valid = std::isfinite(duration_ms) && duration_ms >= 0.0;
    sample.duration_ms = sample.duration_valid ? duration_ms : 0.0;

    std::size_t index = (tick_begin_ + tick_size_) % ticks_.size();
    if (tick_size_ == ticks_.size()) {
        index = tick_begin_;
        tick_begin_ = (tick_begin_ + 1) % ticks_.size();
    }
    else {
        ++tick_size_;
    }
    ticks_[index] = sample;
    last_observation_steady_ms_ = steady_ms;
}

void StatisticsService::recordCpuSnapshot(const CpuSnapshot &current)
{
    if (!started_ || !previous_cpu_.valid || !current.valid || current.wall_ms <= previous_cpu_.wall_ms) {
        previous_cpu_ = current;
        return;
    }

    const CpuUsage usage = cpuUsageBetween(previous_cpu_, current);
    CpuSample sample;
    sample.start_steady_ms = previous_cpu_.wall_ms;
    sample.end_steady_ms = current.wall_ms;
    sample.process = clampUsage(usage.process);
    sample.system = clampUsage(usage.system);
    sample.process_valid = usage.process_valid;
    sample.system_valid = usage.system_valid;

    std::size_t index = (cpu_begin_ + cpu_size_) % cpu_.size();
    if (cpu_size_ == cpu_.size()) {
        index = cpu_begin_;
        cpu_begin_ = (cpu_begin_ + 1) % cpu_.size();
    }
    else {
        ++cpu_size_;
    }
    cpu_[index] = sample;
    previous_cpu_ = current;
    last_observation_steady_ms_ = (std::max)(last_observation_steady_ms_, current.wall_ms);
}

void StatisticsService::recordPlayerCount(long players)
{
    recordPlayerCountAt(players, last_observation_steady_ms_);
}

void StatisticsService::recordPlayerCountAt(long players, std::int64_t steady_ms)
{
    if (!started_ || players < 0) {
        return;
    }

    GaugeSample sample;
    sample.steady_ms = steady_ms;
    sample.players = static_cast<int>((std::min)(players, static_cast<long>((std::numeric_limits<int>::max)())));

    std::size_t index = (gauge_begin_ + gauge_size_) % gauges_.size();
    if (gauge_size_ == gauges_.size()) {
        index = gauge_begin_;
        gauge_begin_ = (gauge_begin_ + 1) % gauges_.size();
    }
    else {
        ++gauge_size_;
    }
    gauges_[index] = sample;
}

void StatisticsService::recordWorldGauges(int entities, int chunks)
{
    recordWorldGaugesAt(entities, chunks, last_observation_steady_ms_);
}

void StatisticsService::recordWorldGaugesAt(int entities, int chunks, std::int64_t steady_ms)
{
    if (!started_ || gauge_size_ == 0) {
        return;
    }
    std::size_t index = (gauge_begin_ + gauge_size_ - 1) % gauges_.size();
    gauges_[index].entities = entities;
    gauges_[index].chunks = chunks;
    gauges_[index].world_gauges_set = true;
}

std::int64_t StatisticsService::effectiveStart(std::int64_t now_ms, std::int64_t window_ms) const
{
    std::int64_t start = (std::max)(start_steady_ms_, now_ms - window_ms);
    if (tick_size_ == ticks_.size()) {
        start = (std::max)(start, ticks_[tick_begin_].steady_ms);
    }
    return (std::min)(start, now_ms);
}

RollingValue StatisticsService::tpsFor(std::int64_t now_ms, std::int64_t window_ms) const
{
    RollingValue result;
    const std::int64_t start = effectiveStart(now_ms, window_ms);
    result.span_ms = now_ms - start;
    if (result.span_ms <= 0) {
        return result;
    }

    for (std::size_t i = 0; i < tick_size_; ++i) {
        const TickSample &sample = ticks_[(tick_begin_ + i) % ticks_.size()];
        if (sample.steady_ms > start && sample.steady_ms <= now_ms) {
            ++result.samples;
        }
    }
    result.present = true;
    result.value = static_cast<double>(result.samples) * 1000.0 / static_cast<double>(result.span_ms);
    return result;
}

DistributionValues StatisticsService::msptFor(std::int64_t now_ms, std::int64_t window_ms) const
{
    DistributionValues result;
    const std::int64_t start = effectiveStart(now_ms, window_ms);
    result.span_ms = now_ms - start;

    std::vector<double> values;
    values.reserve(tick_size_);
    double total = 0.0;
    for (std::size_t i = 0; i < tick_size_; ++i) {
        const TickSample &sample = ticks_[(tick_begin_ + i) % ticks_.size()];
        if (sample.steady_ms > start && sample.steady_ms <= now_ms && sample.duration_valid) {
            values.push_back(sample.duration_ms);
            total += sample.duration_ms;
        }
    }
    if (values.empty()) {
        return result;
    }

    std::sort(values.begin(), values.end());
    result.present = true;
    result.samples = values.size();
    result.mean = total / static_cast<double>(values.size());
    result.min = values.front();
    result.max = values.back();
    const std::size_t middle = values.size() / 2;
    result.median = values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
    const std::size_t percentile_index =
        (std::min)(values.size() - 1,
                   static_cast<std::size_t>(std::ceil(static_cast<double>(values.size()) * 0.95)) - 1);
    result.percentile95 = values[percentile_index];
    return result;
}

RollingValue StatisticsService::cpuFor(std::int64_t now_ms, std::int64_t window_ms, bool process) const
{
    RollingValue result;
    const std::int64_t start = (std::max)(start_steady_ms_, now_ms - window_ms);
    double weighted_total = 0.0;
    std::int64_t covered_ms = 0;

    for (std::size_t i = 0; i < cpu_size_; ++i) {
        const CpuSample &sample = cpu_[(cpu_begin_ + i) % cpu_.size()];
        const bool valid = process ? sample.process_valid : sample.system_valid;
        if (!valid) {
            continue;
        }
        const std::int64_t overlap_start = (std::max)(start, sample.start_steady_ms);
        const std::int64_t overlap_end = (std::min)(now_ms, sample.end_steady_ms);
        if (overlap_end <= overlap_start) {
            continue;
        }
        const std::int64_t overlap_ms = overlap_end - overlap_start;
        weighted_total += (process ? sample.process : sample.system) * static_cast<double>(overlap_ms);
        covered_ms += overlap_ms;
        ++result.samples;
    }

    if (covered_ms <= 0) {
        return result;
    }
    result.present = true;
    result.span_ms = covered_ms;
    result.value = clampUsage(weighted_total / static_cast<double>(covered_ms));
    return result;
}

StatisticsSnapshot StatisticsService::snapshot() const
{
    return snapshotAt(steadyNowMs());
}

StatisticsSnapshot StatisticsService::snapshotAt(std::int64_t steady_ms) const
{
    StatisticsSnapshot result;
    if (!started_) {
        return result;
    }
    const std::int64_t now_ms = (std::max)(steady_ms, last_observation_steady_ms_);
    result.generated_time_ms = unixTimeFor(now_ms);
    result.history_span_ms = (std::min)(kMaximumHistoryMs, now_ms - start_steady_ms_);

    result.tps.last_5s = tpsFor(now_ms, 5 * 1000);
    result.tps.last_10s = tpsFor(now_ms, 10 * 1000);
    result.tps.last_1m = tpsFor(now_ms, 60 * 1000);
    result.tps.last_5m = tpsFor(now_ms, 5 * 60 * 1000);
    result.tps.last_15m = tpsFor(now_ms, 15 * 60 * 1000);

    result.mspt.last_10s = msptFor(now_ms, 10 * 1000);
    result.mspt.last_1m = msptFor(now_ms, 60 * 1000);
    result.mspt.last_5m = msptFor(now_ms, 5 * 60 * 1000);

    result.cpu.process_last_10s = cpuFor(now_ms, 10 * 1000, true);
    result.cpu.process_last_1m = cpuFor(now_ms, 60 * 1000, true);
    result.cpu.process_last_15m = cpuFor(now_ms, 15 * 60 * 1000, true);
    result.cpu.system_last_10s = cpuFor(now_ms, 10 * 1000, false);
    result.cpu.system_last_1m = cpuFor(now_ms, 60 * 1000, false);
    result.cpu.system_last_15m = cpuFor(now_ms, 15 * 60 * 1000, false);
    return result;
}

std::map<std::int32_t, WindowStats> StatisticsService::profileWindows(std::int64_t profile_start_unix_ms,
                                                                      std::int64_t profile_end_unix_ms) const
{
    std::map<std::int32_t, WindowStats> result;
    if (!started_ || profile_end_unix_ms <= profile_start_unix_ms) {
        return result;
    }

    const std::int64_t profile_start_steady = start_steady_ms_ + (profile_start_unix_ms - start_unix_ms_);
    const std::int64_t profile_end_steady = start_steady_ms_ + (profile_end_unix_ms - start_unix_ms_);
    const std::int64_t available_start =
        (std::max)({profile_start_steady, start_steady_ms_, profile_end_steady - kMaximumHistoryMs});
    if (available_start >= profile_end_steady) {
        return result;
    }

    const std::int64_t first_window = (available_start - profile_start_steady) / 1000;
    const std::int64_t last_window = (profile_end_steady - profile_start_steady - 1) / 1000;
    if (first_window > (std::numeric_limits<std::int32_t>::max)() ||
        last_window > (std::numeric_limits<std::int32_t>::max)()) {
        return result;
    }

    struct Accumulator {
        WindowStats stats;
        std::vector<double> durations;
        double process_weighted = 0.0;
        double system_weighted = 0.0;
        std::int64_t process_covered_ms = 0;
        std::int64_t system_covered_ms = 0;
    };
    std::vector<Accumulator> accumulators(static_cast<std::size_t>(last_window - first_window + 1));

    for (std::int64_t window = first_window; window <= last_window; ++window) {
        Accumulator &accumulator = accumulators[static_cast<std::size_t>(window - first_window)];
        const std::int64_t nominal_start = profile_start_steady + window * 1000;
        const std::int64_t start = (std::max)(available_start, nominal_start);
        const std::int64_t end = (std::min)(profile_end_steady, nominal_start + 1000);
        accumulator.stats.ticks_present = true;
        accumulator.stats.tps_present = true;
        accumulator.stats.start_time_ms = unixTimeFor(start);
        accumulator.stats.end_time_ms = unixTimeFor(end);
        accumulator.stats.duration_ms = static_cast<int>((std::max<std::int64_t>)(0, end - start));
    }

    for (std::size_t i = 0; i < tick_size_; ++i) {
        const TickSample &sample = ticks_[(tick_begin_ + i) % ticks_.size()];
        if (sample.steady_ms < available_start || sample.steady_ms >= profile_end_steady) {
            continue;
        }
        const std::int64_t window = (sample.steady_ms - profile_start_steady) / 1000;
        if (window < first_window || window > last_window) {
            continue;
        }
        Accumulator &accumulator = accumulators[static_cast<std::size_t>(window - first_window)];
        ++accumulator.stats.ticks;
        if (sample.duration_valid) {
            accumulator.durations.push_back(sample.duration_ms);
        }
    }

    for (std::size_t i = 0; i < cpu_size_; ++i) {
        const CpuSample &sample = cpu_[(cpu_begin_ + i) % cpu_.size()];
        for (std::int64_t window = first_window; window <= last_window; ++window) {
            Accumulator &accumulator = accumulators[static_cast<std::size_t>(window - first_window)];
            const std::int64_t window_start = start_steady_ms_ + (accumulator.stats.start_time_ms - start_unix_ms_);
            const std::int64_t window_end = start_steady_ms_ + (accumulator.stats.end_time_ms - start_unix_ms_);
            const std::int64_t overlap_start = (std::max)(window_start, sample.start_steady_ms);
            const std::int64_t overlap_end = (std::min)(window_end, sample.end_steady_ms);
            if (overlap_end <= overlap_start) {
                continue;
            }
            const std::int64_t overlap_ms = overlap_end - overlap_start;
            if (sample.process_valid) {
                accumulator.process_weighted += sample.process * static_cast<double>(overlap_ms);
                accumulator.process_covered_ms += overlap_ms;
            }
            if (sample.system_valid) {
                accumulator.system_weighted += sample.system * static_cast<double>(overlap_ms);
                accumulator.system_covered_ms += overlap_ms;
            }
        }
    }

    for (std::int64_t window = first_window; window <= last_window; ++window) {
        Accumulator &accumulator = accumulators[static_cast<std::size_t>(window - first_window)];
        WindowStats &stats = accumulator.stats;
        if (stats.duration_ms > 0) {
            stats.tps = static_cast<double>(stats.ticks) * 1000.0 / static_cast<double>(stats.duration_ms);
        }

        if (!accumulator.durations.empty()) {
            std::sort(accumulator.durations.begin(), accumulator.durations.end());
            stats.mspt_present = true;
            stats.mspt_max = accumulator.durations.back();
            const std::size_t middle = accumulator.durations.size() / 2;
            stats.mspt_median = accumulator.durations.size() % 2 == 0
                                  ? (accumulator.durations[middle - 1] + accumulator.durations[middle]) / 2.0
                                  : accumulator.durations[middle];
        }
        if (accumulator.process_covered_ms > 0) {
            stats.cpu_process_present = true;
            stats.cpu_process = accumulator.process_weighted / static_cast<double>(accumulator.process_covered_ms);
        }
        if (accumulator.system_covered_ms > 0) {
            stats.cpu_system_present = true;
            stats.cpu_system = accumulator.system_weighted / static_cast<double>(accumulator.system_covered_ms);
        }

        for (std::size_t i = 0; i < gauge_size_; ++i) {
            const GaugeSample &gauge = gauges_[(gauge_begin_ + i) % gauges_.size()];
            if (gauge.steady_ms <= start_steady_ms_ + (stats.end_time_ms - start_unix_ms_)) {
                stats.players_present = true;
                stats.players = gauge.players;
                if (gauge.world_gauges_set) {
                    stats.entities_present = true;
                    stats.entities = gauge.entities;
                    stats.chunks_present = true;
                    stats.chunks = gauge.chunks;
                }
            }
        }
        result.emplace(static_cast<std::int32_t>(window), stats);
    }
    return result;
}

std::int64_t StatisticsService::unixTimeFor(std::int64_t steady_ms) const
{
    return start_unix_ms_ + (steady_ms - start_steady_ms_);
}

}  // namespace spark
