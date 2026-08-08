#include "core/stats/network_monitor.h"

#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <cassert>
#include <cstdio>

using namespace spark;

namespace {

// Mock poll function that returns a sequence of predefined readings.
class MockPoller {
public:
    void addReading(const std::map<std::string, NetworkInterfaceInfo> &reading)
    {
        readings_.push_back(reading);
    }

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

NetworkInterfaceInfo makeInfo(const std::string &name,
                              std::int64_t rx_bytes, std::int64_t rx_packets,
                              std::int64_t tx_bytes, std::int64_t tx_packets)
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

    NetworkMonitor monitor(std::ref(poller));
    monitor.poll();  // first poll, returns false
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

    NetworkMonitor monitor(std::ref(poller));
    monitor.poll();
    monitor.poll();

    auto snap = monitor.snapshot();
    assert(snap.count("eth0") == 1);
    assert(snap.count("veth1234") == 0);
    assert(snap.count("br-abc") == 0);
}

void testNetworkMonitorEmptyPolls()
{
    MockPoller poller;
    NetworkMonitor monitor(std::ref(poller));
    assert(!monitor.poll());
    assert(!monitor.poll());
    assert(monitor.snapshot().empty());
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

    std::printf("All network_monitor tests passed.\n");
    return 0;
}
