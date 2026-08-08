#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/stats/network_monitor.h"

using namespace spark;  // NOLINT(google-build-using-namespace)

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "network monitor: %s\n", message);
        std::abort();
    }
}

// Mock poll function that returns a sequence of predefined readings.
class MockPoller {
public:
    void addReading(const std::map<std::string, NetworkInterfaceInfo> &reading) { readings_.push_back(reading); }

    std::map<std::string, NetworkInterfaceInfo> operator()()
    {
        if (index_ >= readings_.size()) {
            return {};
        }
        return readings_[index_++];
    }

private:
    std::vector<std::map<std::string, NetworkInterfaceInfo>> readings_;
    std::size_t index_ = 0;
};

class MockClock {
public:
    NetworkMonitor::Clock::time_point operator()() const { return now_; }
    void advance(std::chrono::nanoseconds duration) { now_ += duration; }

private:
    NetworkMonitor::Clock::time_point now_;
};

NetworkInterfaceInfo makeInfo(const std::string &name, std::uint64_t rx_bytes, std::uint64_t rx_packets,
                              std::uint64_t tx_bytes, std::uint64_t tx_packets)
{
    NetworkInterfaceInfo info;
    info.name = name;
    info.rx_bytes = rx_bytes;
    info.rx_packets = rx_packets;
    info.tx_bytes = tx_bytes;
    info.tx_packets = tx_packets;
    return info;
}

void testSubtract()
{
    NetworkInterfaceInfo a = makeInfo("eth0", 1000, 100, 2000, 200);
    NetworkInterfaceInfo b = makeInfo("eth0", 600, 60, 1200, 120);
    NetworkInterfaceInfo diff = a.subtract(b);
    assert(diff.rx_bytes == 400);
    assert(diff.rx_packets == 40);
    assert(diff.tx_bytes == 800);
    assert(diff.tx_packets == 80);

    // Subtracting zero returns the original.
    NetworkInterfaceInfo zero;
    NetworkInterfaceInfo diff2 = a.subtract(zero);
    assert(diff2.rx_bytes == 1000);
    assert(diff2.tx_bytes == 2000);

    // isZero
    assert(zero.isZero());
    assert(!a.isZero());
}

void testDoubleRollingAverageEmpty()
{
    DoubleRollingAverage ra(5);
    assert(ra.samples() == 0);
    assert(ra.mean() == 0.0);
    assert(ra.max() == 0.0);
    assert(ra.min() == 0.0);
    assert(ra.median() == 0.0);
    assert(ra.percentile95() == 0.0);
}

void testDoubleRollingAverageBasic()
{
    DoubleRollingAverage ra(5);
    ra.add(10.0);
    ra.add(20.0);
    ra.add(30.0);
    assert(ra.samples() == 3);
    assert(std::abs(ra.mean() - 20.0) < 0.001);
    assert(ra.min() == 10.0);
    assert(ra.max() == 30.0);
    // median of [10,20,30] = 20
    assert(std::abs(ra.median() - 20.0) < 0.001);
    // p95: ceil(0.95 * 2) = 2 -> sorted[2] = 30
    assert(std::abs(ra.percentile95() - 30.0) < 0.001);
}

void testDoubleRollingAverageOverflow()
{
    DoubleRollingAverage ra(3);
    ra.add(1.0);
    ra.add(2.0);
    ra.add(3.0);
    ra.add(4.0);  // overwrites 1.0
    assert(ra.samples() == 3);
    // Window now contains [4, 2, 3], mean = 3
    assert(std::abs(ra.mean() - 3.0) < 0.001);
    assert(ra.min() == 2.0);
    assert(ra.max() == 4.0);
    // sorted = [2, 3, 4], median = 3
    assert(std::abs(ra.median() - 3.0) < 0.001);
}

void testNetworkMonitorFirstPoll()
{
    MockPoller poller;
    poller.addReading({{"eth0", makeInfo("eth0", 1000, 10, 2000, 20)}});

    NetworkMonitor monitor(std::ref(poller));
    bool result = monitor.poll();
    assert(!result);  // first poll returns false

    auto totals = monitor.systemTotals();
    assert(totals.size() == 1);
    assert(totals.count("eth0") == 1);
}

void testNetworkMonitorSecondPoll()
{
    MockPoller poller;
    poller.addReading({{"eth0", makeInfo("eth0", 1000, 10, 2000, 20)}});
    poller.addReading({{"eth0", makeInfo("eth0", 7000, 70, 8000, 80)}});

    MockClock clock;
    NetworkMonitor monitor(std::ref(poller), std::ref(clock));
    monitor.poll();  // first poll, returns false
    clock.advance(std::chrono::seconds(60));
    bool result = monitor.poll();
    assert(result);  // second poll returns true

    auto snap = monitor.snapshot();
    assert(snap.count("eth0") == 1);
    const auto &s = snap["eth0"];
    assert(s.rx_bytes_per_second.present);
    // diff = 6000 bytes over 60s = 100 bytes/s
    assert(std::abs(s.rx_bytes_per_second.mean - 100.0) < 0.001);
    assert(std::abs(s.tx_bytes_per_second.mean - 100.0) < 0.001);
    assert(std::abs(s.rx_packets_per_second.mean - 1.0) < 0.001);
    assert(std::abs(s.tx_packets_per_second.mean - 1.0) < 0.001);
}

void testNetworkMonitorIgnoresVethAndBr()
{
    MockPoller poller;
    poller.addReading({
        {"eth0", makeInfo("eth0", 1000, 10, 2000, 20)},
        {"veth1234", makeInfo("veth1234", 500, 5, 500, 5)},
        {"br-abc", makeInfo("br-abc", 300, 3, 300, 3)},
    });
    poller.addReading({
        {"eth0", makeInfo("eth0", 7000, 70, 8000, 80)},
        {"veth1234", makeInfo("veth1234", 1000, 10, 1000, 10)},
        {"br-abc", makeInfo("br-abc", 600, 6, 600, 6)},
    });

    MockClock clock;
    NetworkMonitor monitor(std::ref(poller), std::ref(clock));
    monitor.poll();
    clock.advance(std::chrono::seconds(60));
    monitor.poll();

    auto snap = monitor.snapshot();
    assert(snap.count("eth0") == 1);
    assert(!snap.contains("veth1234"));
    assert(!snap.contains("br-abc"));
}

void testNetworkMonitorEmptyPolls()
{
    MockPoller poller;
    NetworkMonitor monitor(std::ref(poller));
    assert(!monitor.poll());
    assert(!monitor.poll());
    assert(monitor.snapshot().empty());
}

void testNetworkMonitorResetAndRebaseline()
{
    MockPoller poller;
    poller.addReading({{"eth0", makeInfo("eth0", 1000, 100, 2000, 200)}});
    poller.addReading({{"eth0", makeInfo("eth0", 2000, 200, 3000, 300)}});
    poller.addReading({{"eth0", makeInfo("eth0", 100, 10, 200, 20)}});
    poller.addReading({{"eth0", makeInfo("eth0", 1100, 110, 1200, 120)}});

    MockClock clock;
    NetworkMonitor monitor(std::ref(poller), std::ref(clock));
    require(!monitor.poll(), "initial reset baseline was accepted");
    clock.advance(std::chrono::seconds(10));
    require(monitor.poll(), "normal reset prelude delta was rejected");
    clock.advance(std::chrono::seconds(10));
    require(!monitor.poll(), "counter reset was accepted as a delta");
    clock.advance(std::chrono::seconds(10));
    require(monitor.poll(), "post-reset delta was rejected");
    const auto snapshot = monitor.snapshot();
    require(snapshot.at("eth0").rx_bytes_per_second.min >= 0.0, "reset produced a negative rate");
    require(std::abs(snapshot.at("eth0").rx_bytes_per_second.mean - 100.0) < 0.001,
            "post-reset rate used the absolute counter");
}

void testNetworkMonitorInterfaceLifecycle()
{
    MockPoller poller;
    poller.addReading({{"eth0", makeInfo("eth0", 1000, 10, 1000, 10)}});
    poller.addReading(
        {{"eth0", makeInfo("eth0", 1100, 11, 1100, 11)}, {"wlan0", makeInfo("wlan0", 1000000, 1000, 1000000, 1000)}});
    poller.addReading({});
    poller.addReading({{"wlan0", makeInfo("wlan0", 2000000, 2000, 2000000, 2000)}});
    poller.addReading({{"wlan0", makeInfo("wlan0", 2000100, 2001, 2000100, 2001)}});

    MockClock clock;
    NetworkMonitor monitor(std::ref(poller), std::ref(clock));
    require(!monitor.poll(), "initial interface baseline was accepted");
    clock.advance(std::chrono::seconds(10));
    require(monitor.poll(), "existing interface delta was rejected");
    require(!monitor.snapshot().contains("wlan0"), "new interface absolute counter was accepted");
    clock.advance(std::chrono::seconds(10));
    require(!monitor.poll(), "disappeared interface produced a rate");
    require(!monitor.snapshot().contains("eth0"), "disappeared interface retained stale rates");
    clock.advance(std::chrono::seconds(10));
    require(!monitor.poll(), "reappeared interface absolute counter was accepted");
    clock.advance(std::chrono::seconds(10));
    require(monitor.poll(), "reappeared interface delta was rejected");
    require(std::abs(monitor.snapshot().at("wlan0").rx_bytes_per_second.mean - 10.0) < 0.001,
            "reappeared interface rate was incorrect");
}

void testNetworkMonitorElapsedGuard()
{
    MockPoller poller;
    poller.addReading({{"eth0", makeInfo("eth0", 100, 10, 100, 10)}});
    poller.addReading({{"eth0", makeInfo("eth0", 200, 20, 200, 20)}});
    poller.addReading({{"eth0", makeInfo("eth0", 201, 21, 201, 21)}});

    MockClock clock;
    NetworkMonitor monitor(std::ref(poller), std::ref(clock));
    require(!monitor.poll(), "initial elapsed baseline was accepted");
    require(!monitor.poll(), "zero elapsed interval was accepted");
    clock.advance(std::chrono::microseconds(1));
    require(monitor.poll(), "small positive elapsed interval was rejected");
    const double rate = monitor.snapshot().at("eth0").rx_bytes_per_second.mean;
    require(std::isfinite(rate), "small elapsed interval produced a non-finite rate");
    require(rate >= 0.0, "small elapsed interval produced a negative rate");
}

}  // namespace

int main()
{
    testSubtract();
    testDoubleRollingAverageEmpty();
    testDoubleRollingAverageBasic();
    testDoubleRollingAverageOverflow();
    testNetworkMonitorFirstPoll();
    testNetworkMonitorSecondPoll();
    testNetworkMonitorIgnoresVethAndBr();
    testNetworkMonitorEmptyPolls();
    testNetworkMonitorResetAndRebaseline();
    testNetworkMonitorInterfaceLifecycle();
    testNetworkMonitorElapsedGuard();

    std::printf("All network_monitor tests passed.\n");
    return 0;
}
