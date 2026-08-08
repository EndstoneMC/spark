#include <cassert>
#include <map>
#include <string>
#include <vector>

#include "core/stats/ping_statistics.h"

using namespace spark;

namespace {

// A simple mock ping provider for deterministic testing.
class MockPingProvider : public PlayerPingProvider {
public:
    explicit MockPingProvider(std::map<std::string, int> pings) : pings_(std::move(pings)) {}

    std::map<std::string, int> poll() override { return pings_; }

private:
    std::map<std::string, int> pings_;
};

void testPingSummary()
{
    // Empty
    PingSummary empty;
    assert(empty.total() == 0);
    assert(empty.min() == 0);
    assert(empty.median() == 0);
    assert(empty.percentile95th() == 0);
    assert(empty.max() == 0);

    // Single value
    PingSummary single({42});
    assert(single.total() == 1);
    assert(single.min() == 42);
    assert(single.median() == 42);
    assert(single.percentile95th() == 42);
    assert(single.max() == 42);

    // Multiple values - [10, 20, 30, 40, 100]
    PingSummary multi({100, 10, 30, 20, 40});
    assert(multi.total() == 5);
    assert(multi.min() == 10);
    // median: ceil(0.50 * 4) = 2 -> index 2 -> 30
    assert(multi.median() == 30);
    // p95: ceil(0.95 * 4) = ceil(3.8) = 4 -> index 4 -> 100
    assert(multi.percentile95th() == 100);
    assert(multi.max() == 100);

    // Two values - [10, 100]
    PingSummary two({100, 10});
    assert(two.total() == 2);
    assert(two.min() == 10);
    // median: ceil(0.50 * 1) = 1 -> index 1 -> 100
    assert(two.median() == 100);
    // p95: ceil(0.95 * 1) = 1 -> index 1 -> 100
    assert(two.percentile95th() == 100);
    assert(two.max() == 100);
}

void testPingRollingAverage()
{
    PingRollingAverage ra(3);
    assert(ra.samples() == 0);
    assert(ra.mean() == 0.0);

    ra.add(10);
    assert(ra.samples() == 1);
    assert(ra.min() == 10);
    assert(ra.max() == 10);

    ra.add(20);
    assert(ra.samples() == 2);
    assert(ra.mean() == 15.0);
    assert(ra.min() == 10);
    assert(ra.max() == 20);
    // median: ceil(0.50 * 1) = 1 -> sorted [10, 20] -> index 1 -> 20
    assert(ra.median() == 20);

    ra.add(30);
    assert(ra.samples() == 3);
    assert(ra.mean() == 20.0);
    assert(ra.min() == 10);
    // median: ceil(0.50 * 2) = 1 -> sorted [10, 20, 30] -> index 1 -> 20
    assert(ra.median() == 20);
    assert(ra.max() == 30);

    // Overflow: ring buffer wraps, overwriting oldest (10)
    ra.add(40);
    assert(ra.samples() == 3);
    // Samples are now [40, 20, 30] (10 was overwritten at head 0)
    assert(ra.min() == 20);
    assert(ra.max() == 40);
}

void testPingStatistics()
{
    MockPingProvider provider({{"Alice", 50}, {"Bob", 100}, {"Charlie", 200}});
    PingStatistics stats(provider);

    // Current summary
    PingSummary summary = stats.currentSummary();
    assert(summary.total() == 3);
    assert(summary.min() == 50);
    assert(summary.median() == 100);
    assert(summary.max() == 200);

    // Poll - should add median to rolling average
    assert(stats.poll());
    assert(stats.rollingAverage().samples() == 1);
    assert(stats.rollingAverage().median() == 100);

    // Poll again
    assert(stats.poll());
    assert(stats.rollingAverage().samples() == 2);

    // Query specific player - exact match
    PlayerPing alice = stats.query("Alice");
    assert(alice.found());
    assert(alice.name == "Alice");
    assert(alice.ping == 50);

    // Case-insensitive match
    PlayerPing bob = stats.query("bob");
    assert(bob.found());
    assert(bob.name == "Bob");
    assert(bob.ping == 100);

    // Not found
    PlayerPing nobody = stats.query("Nobody");
    assert(!nobody.found());
}

void testPingStatisticsEmpty()
{
    MockPingProvider provider({});
    PingStatistics stats(provider);

    PingSummary summary = stats.currentSummary();
    assert(summary.total() == 0);

    // Poll with no players should not add to rolling average
    assert(!stats.poll());
    assert(stats.rollingAverage().samples() == 0);
}

void testPingStatisticsZeroPing()
{
    // Players with ping <= 0 should be filtered out
    MockPingProvider provider({{"Zero", 0}, {"Bob", 100}});
    PingStatistics stats(provider);

    PingSummary summary = stats.currentSummary();
    assert(summary.total() == 1);
    assert(summary.min() == 100);
    assert(summary.max() == 100);
}

}  // namespace

int main()
{
    testPingSummary();
    testPingRollingAverage();
    testPingStatistics();
    testPingStatisticsEmpty();
    testPingStatisticsZeroPing();

    printf("All ping statistics tests passed.\n");
    return 0;
}
