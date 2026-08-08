#ifndef SPARK_CORE_RECOVERY_STALL_WATCHDOG_H
#define SPARK_CORE_RECOVERY_STALL_WATCHDOG_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "native/sampler/heartbeat.h"

namespace spark {

// Independent-thread watchdog that monitors the main-thread heartbeat and
// transitions between Healthy and Stalled when the server tick stops advancing.
// The watchdog never calls Endstone APIs, never stops the profiler, and never
// performs file I/O.  State transitions fire the stall callback exactly once
// per edge (STALL_BEGIN / STALL_END).
//
// State machine:
//   Healthy  --stall-->  Stalled   (callback(true))
//   Stalled  --recover--> Healthy  (callback(false))
//   *        --shutdown--> Stopping (no callback, terminal)
class StallWatchdog {
public:
    // Production thresholds.
    static constexpr std::uint64_t kStallThresholdNs = 5'000'000'000ULL;    // 5 s
    static constexpr std::uint64_t kSevereThresholdNs = 15'000'000'000ULL;  // 15 s
    static constexpr int kPollIntervalMs = 500;

    enum class State : std::uint8_t {
        Healthy = 0,
        Stalled = 1,
        Stopping = 2,
    };

    // Called with true when a stall begins, false when it ends.
    using StallCallback = std::function<void(bool stalled)>;

    // Constructed with the server (main-thread) heartbeat.  Sampler and
    // aggregator heartbeats are optional diagnostic inputs.
    explicit StallWatchdog(Heartbeat &server_hb,
                           std::uint64_t stall_threshold_ns = kStallThresholdNs,
                           int poll_interval_ms = kPollIntervalMs);
    ~StallWatchdog();

    StallWatchdog(const StallWatchdog &) = delete;
    StallWatchdog &operator=(const StallWatchdog &) = delete;

    // Sets optional diagnostic heartbeats.  Must be called before start().
    void setSamplerHeartbeat(const Heartbeat *hb) { sampler_hb_ = hb; }
    void setAggregatorHeartbeat(const Heartbeat *hb) { aggregator_hb_ = hb; }

    void setStallCallback(StallCallback cb);

    // Spawns the watchdog thread.  Safe to call once.
    void start();

    // Signals the thread to stop and joins it.  State becomes Stopping.
    void stop();

    State state() const { return state_.load(std::memory_order_acquire); }
    bool stalled() const { return state_.load(std::memory_order_acquire) == State::Stalled; }

private:
    void loop();

    Heartbeat &server_hb_;
    const Heartbeat *sampler_hb_ = nullptr;
    const Heartbeat *aggregator_hb_ = nullptr;

    const std::uint64_t stall_threshold_ns_;
    const int poll_interval_ms_;

    std::atomic<bool> running_{false};
    std::atomic<State> state_{State::Healthy};
    std::thread thread_;

    StallCallback callback_;
    std::mutex callback_mutex_;
};

}  // namespace spark

#endif  // SPARK_CORE_RECOVERY_STALL_WATCHDOG_H
