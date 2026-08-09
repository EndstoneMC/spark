#include "core/recovery/stall_watchdog.h"

#include <chrono>
#include <utility>

namespace spark {

StallWatchdog::StallWatchdog(Heartbeat &server_hb, std::uint64_t stall_threshold_ns, int poll_interval_ms)
    : server_hb_(server_hb), stall_threshold_ns_(stall_threshold_ns), poll_interval_ms_(poll_interval_ms)
{
}

StallWatchdog::~StallWatchdog()
{
    stop();
}

void StallWatchdog::setStallCallback(StallCallback cb)
{
    std::scoped_lock lock(callback_mutex_);
    callback_ = std::move(cb);
}

void StallWatchdog::start()
{
    if (running_.exchange(true)) {
        return;
    }
    state_.store(State::Healthy, std::memory_order_release);
    try {
        thread_ = std::thread([this] {
            try {
                loop();
            }
            catch (...) {
                running_.store(false, std::memory_order_release);
            }
        });
    }
    catch (...) {
        running_.store(false);
    }
}

void StallWatchdog::stop()
{
    running_.store(false);
    state_.store(State::Stopping, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void StallWatchdog::loop()
{
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms_));
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }

        const std::uint64_t last = server_hb_.last_ns.load(std::memory_order_acquire);
        // No tick yet — cannot determine stall.
        if (last == 0) {
            continue;
        }

        const std::uint64_t now = Heartbeat::monotonicNowNs();
        const std::uint64_t elapsed = now - last;

        if (elapsed >= stall_threshold_ns_) {
            State expected = State::Healthy;
            if (state_.compare_exchange_strong(expected, State::Stalled, std::memory_order_acq_rel)) {
                std::scoped_lock lock(callback_mutex_);
                if (callback_) {
                    callback_(true);
                }
            }
        }
        else {
            State expected = State::Stalled;
            if (state_.compare_exchange_strong(expected, State::Healthy, std::memory_order_acq_rel)) {
                std::scoped_lock lock(callback_mutex_);
                if (callback_) {
                    callback_(false);
                }
            }
        }
    }
}

}  // namespace spark
