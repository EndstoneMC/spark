#ifndef SPARK_NATIVE_SAMPLER_HEARTBEAT_H
#define SPARK_NATIVE_SAMPLER_HEARTBEAT_H

#include <atomic>
#include <chrono>
#include <cstdint>

namespace spark {

// Lightweight atomic heartbeat for stall detection.  Updated from hot paths
// (main-thread tick, sampler loop, aggregator loop) and read by the watchdog
// thread.  Must remain allocation-free, lock-free, and I/O-free.
struct Heartbeat {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> last_ns{0};

    void beat() noexcept
    {
        last_ns.store(monotonicNowNs(), std::memory_order_release);
        sequence.fetch_add(1, std::memory_order_release);
    }

    static std::uint64_t monotonicNowNs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};

}  // namespace spark

#endif  // SPARK_NATIVE_SAMPLER_HEARTBEAT_H
