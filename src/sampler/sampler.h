#ifndef ENDSTONE_SPARK_SAMPLER_H
#define ENDSTONE_SPARK_SAMPLER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

#include <moodycamel/concurrentqueue.h>

#include "sampler/call_tree.h"
#include "sampler/types.h"

namespace spark {

struct SamplerConfig {
    int interval_us = 4000;
    bool ignore_sleeping = true;
    std::int64_t only_ticks_over_ms = 0;  // 0 = disabled (record every tick)
};

// Per-window tick accounting, used to build the viewer's timeline overlay.
struct WindowTickStats {
    int ticks = 0;
    double mspt_sum = 0.0;
    double mspt_max = 0.0;
};

// Wall-clock sampling profiler for a single target thread. The sampler thread
// captures stacks and hands them to an aggregator thread via a lock-free queue;
// the aggregator builds the call tree, bucketing per tick so --only-ticks-over can
// drop fast ticks. Samples are stamped with a per-second time window so the viewer
// can scrub over time. Nothing here touches the Endstone API.
class Sampler {
public:
    ~Sampler();

    bool start(const SamplerConfig &config);  // arms capture + spawns threads
    void stop();                              // stops + joins; safe to call once

    void setTarget(std::uint64_t tid)
    {
        target_tid_.store(tid);
    }

    // Called once per server tick from the main thread: `mspt_ms` is the duration
    // of the tick that just finished.
    void onTick(double mspt_ms);

    // Valid after stop(): the aggregated data.
    const CallTree &tree() const
    {
        return tree_;
    }
    const ModuleTable &modules() const
    {
        return modules_;
    }
    const std::map<std::int32_t, WindowTickStats> &windowTicks() const
    {
        return window_ticks_;
    }
    std::uint64_t numberOfTicks() const
    {
        return current_tick_.load();
    }
    std::uint64_t sampleCount() const
    {
        return tree_.sampleCount();
    }

private:
    struct TickEvent {
        std::uint64_t tick_id;
        double mspt_ms;
    };

    void samplerLoop();
    void aggregatorLoop();
    void flushOrDrop(std::uint64_t tick_id, bool keep);
    std::int32_t currentWindow() const;

    SamplerConfig config_;
    std::atomic<bool> running_{false};      // sampler (producer) thread
    std::atomic<bool> agg_running_{false};  // aggregator (consumer) thread
    std::atomic<std::uint64_t> target_tid_{0};
    std::atomic<std::uint64_t> current_tick_{0};
    std::chrono::steady_clock::time_point start_time_{};

    std::thread sampler_thread_;
    std::thread aggregator_thread_;

    moodycamel::ConcurrentQueue<Sample> samples_;
    moodycamel::ConcurrentQueue<TickEvent> ticks_;

    // aggregator-thread state
    CallTree tree_;
    std::unordered_map<std::uint64_t, std::vector<Sample>> buckets_;

    // sampler-thread state
    ModuleTable modules_;

    // main-thread state (written by onTick, read at export after join)
    std::map<std::int32_t, WindowTickStats> window_ticks_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_SAMPLER_H
