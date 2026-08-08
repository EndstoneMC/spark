#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include "core/recovery/stall_watchdog.h"

using namespace spark;

namespace {

// Short thresholds for fast tests.
constexpr std::uint64_t kTestStallNs = 200'000'000ULL;  // 200 ms
constexpr int kTestPollMs = 50;

class WatchdogFixture {
public:
    WatchdogFixture() : watchdog_(server_hb_, kTestStallNs, kTestPollMs) {}
    ~WatchdogFixture() { watchdog_.stop(); }

    void start() { watchdog_.start(); }
    StallWatchdog &watchdog() { return watchdog_; }
    Heartbeat &serverHb() { return server_hb_; }

    int stallBeginCount() const { return stall_begin_.load(); }
    int stallEndCount() const { return stall_end_.load(); }

    void installCallback()
    {
        watchdog_.setStallCallback([this](bool stalled) {
            if (stalled) {
                stall_begin_.fetch_add(1);
            }
            else {
                stall_end_.fetch_add(1);
            }
        });
    }

private:
    Heartbeat server_hb_;
    StallWatchdog watchdog_;
    std::atomic<int> stall_begin_{0};
    std::atomic<int> stall_end_{0};
};

void testStartsHealthy()
{
    WatchdogFixture f;
    assert(f.watchdog().state() == StallWatchdog::State::Healthy);
    std::cout << "testStartsHealthy: PASS\n";
}

void testStopTransitionsToStopping()
{
    WatchdogFixture f;
    f.start();
    f.watchdog().stop();
    assert(f.watchdog().state() == StallWatchdog::State::Stopping);
    std::cout << "testStopTransitionsToStopping: PASS\n";
}

void testNoFalseStallBeforeFirstTick()
{
    WatchdogFixture f;
    f.installCallback();
    f.start();
    // Wait well beyond the stall threshold without any heartbeat.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    assert(f.watchdog().state() == StallWatchdog::State::Healthy);
    assert(f.stallBeginCount() == 0);
    std::cout << "testNoFalseStallBeforeFirstTick: PASS\n";
}

void testHeartbeatStaysHealthy()
{
    WatchdogFixture f;
    f.installCallback();
    f.start();
    for (int i = 0; i < 10; ++i) {
        f.serverHb().beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(f.watchdog().state() == StallWatchdog::State::Healthy);
    assert(f.stallBeginCount() == 0);
    std::cout << "testHeartbeatStaysHealthy: PASS\n";
}

void testStallDetected()
{
    WatchdogFixture f;
    f.installCallback();
    f.start();
    f.serverHb().beat();
    // Wait for stall threshold (200 ms + poll margin).
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    assert(f.watchdog().state() == StallWatchdog::State::Stalled);
    assert(f.stallBeginCount() == 1);
    std::cout << "testStallDetected: PASS\n";
}

void testStallBeginFiresOnce()
{
    WatchdogFixture f;
    f.installCallback();
    f.start();
    f.serverHb().beat();
    // Wait long enough for multiple polls past the threshold.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    assert(f.stallBeginCount() == 1);
    std::cout << "testStallBeginFiresOnce: PASS\n";
}

void testRecoveryTransitionsToHealthy()
{
    WatchdogFixture f;
    f.installCallback();
    f.start();
    f.serverHb().beat();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    assert(f.watchdog().state() == StallWatchdog::State::Stalled);
    assert(f.stallBeginCount() == 1);
    assert(f.stallEndCount() == 0);

    // Resume ticking.
    for (int i = 0; i < 5; ++i) {
        f.serverHb().beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(f.watchdog().state() == StallWatchdog::State::Healthy);
    assert(f.stallEndCount() == 1);
    std::cout << "testRecoveryTransitionsToHealthy: PASS\n";
}

void testMultipleStallCycles()
{
    WatchdogFixture f;
    f.installCallback();
    f.start();

    // First stall.
    f.serverHb().beat();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    assert(f.watchdog().state() == StallWatchdog::State::Stalled);
    assert(f.stallBeginCount() == 1);

    // Recover.
    for (int i = 0; i < 5; ++i) {
        f.serverHb().beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(f.watchdog().state() == StallWatchdog::State::Healthy);
    assert(f.stallEndCount() == 1);

    // Second stall.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    assert(f.watchdog().state() == StallWatchdog::State::Stalled);
    assert(f.stallBeginCount() == 2);

    // Second recovery.
    for (int i = 0; i < 5; ++i) {
        f.serverHb().beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(f.watchdog().state() == StallWatchdog::State::Healthy);
    assert(f.stallEndCount() == 2);
    std::cout << "testMultipleStallCycles: PASS\n";
}

void testStopDuringStall()
{
    WatchdogFixture f;
    f.installCallback();
    f.start();
    f.serverHb().beat();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    assert(f.watchdog().state() == StallWatchdog::State::Stalled);

    f.watchdog().stop();
    assert(f.watchdog().state() == StallWatchdog::State::Stopping);
    std::cout << "testStopDuringStall: PASS\n";
}

void testStopIsIdempotent()
{
    WatchdogFixture f;
    f.start();
    f.watchdog().stop();
    f.watchdog().stop();
    assert(f.watchdog().state() == StallWatchdog::State::Stopping);
    std::cout << "testStopIsIdempotent: PASS\n";
}

void testStartIsIdempotent()
{
    WatchdogFixture f;
    f.start();
    f.start();
    f.watchdog().stop();
    assert(f.watchdog().state() == StallWatchdog::State::Stopping);
    std::cout << "testStartIsIdempotent: PASS\n";
}

void testDestructorStopsThread()
{
    Heartbeat hb;
    {
        StallWatchdog wd(hb, kTestStallNs, kTestPollMs);
        wd.start();
        assert(wd.state() == StallWatchdog::State::Healthy);
    }
    // If the destructor didn't join, the test would likely crash or hang.
    std::cout << "testDestructorStopsThread: PASS\n";
}

}  // namespace

int main()
{
    testStartsHealthy();
    testStopTransitionsToStopping();
    testNoFalseStallBeforeFirstTick();
    testHeartbeatStaysHealthy();
    testStallDetected();
    testStallBeginFiresOnce();
    testRecoveryTransitionsToHealthy();
    testMultipleStallCycles();
    testStopDuringStall();
    testStopIsIdempotent();
    testStartIsIdempotent();
    testDestructorStopsThread();
    std::cout << "All stall watchdog tests passed.\n";
    return 0;
}
