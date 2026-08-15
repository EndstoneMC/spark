#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/stats/statistics_service.h"

namespace {

spark::CpuSnapshot initialCpu()
{
    spark::CpuSnapshot result;
    result.valid = true;
    result.process_ticks_per_second = 100.0;
    result.cpu_threads = 2;
    return result;
}

spark::DistributionValues distribution(const std::vector<double> &values, std::size_t window)
{
    spark::StatisticsService statistics;
    statistics.startAt(0, 0, initialCpu());
    std::int64_t steady_ms = 0;
    for (double value : values) {
        statistics.recordTickAt(value, ++steady_ms);
    }
    return statistics.placeholderTickDuration(window);
}

void requireJavaRanks(const std::vector<double> &values, double median, double percentile95)
{
    const auto actual = distribution(values, values.size());
    assert(actual.present);
    assert(actual.samples == values.size());
    assert(actual.median == median);
    assert(actual.percentile95 == percentile95);
}

}  // namespace

int main()
{
    assert(!spark::StatisticsService{}.placeholderTickDuration(200).present);

    requireJavaRanks({7.0}, 7.0, 7.0);
    requireJavaRanks({1.0, 9.0}, 9.0, 9.0);
    requireJavaRanks({1.0, 5.0, 9.0}, 5.0, 9.0);
    requireJavaRanks({1.0, 3.0, 7.0, 9.0}, 7.0, 9.0);
    requireJavaRanks({1.0, 3.0, 5.0, 7.0, 9.0}, 5.0, 9.0);

    std::vector<double> percentile_values;
    for (int value = 1; value <= 20; ++value) {
        percentile_values.push_back(static_cast<double>(value));
    }
    requireJavaRanks(percentile_values, 11.0, 20.0);

    spark::StatisticsService statistics;
    statistics.startAt(0, 0, initialCpu());
    for (int tick = 1; tick <= 1300; ++tick) {
        const double duration = tick <= 100 ? 1000.0 : static_cast<double>(tick);
        statistics.recordTickAt(duration, static_cast<std::int64_t>(tick) * 1000);
    }

    const auto last_200 = statistics.placeholderTickDuration(200);
    const auto last_1200 = statistics.placeholderTickDuration(1200);
    assert(last_200.samples == 200);
    assert(last_200.min == 1101.0);
    assert(last_200.median == 1201.0);
    assert(last_200.percentile95 == 1291.0);
    assert(last_200.max == 1300.0);
    assert(last_1200.samples == 1200);
    assert(last_1200.min == 101.0);
    assert(last_1200.median == 701.0);
    assert(last_1200.percentile95 == 1241.0);
    assert(last_1200.max == 1300.0);

    const auto partial = distribution({39.9, 40.0, 49.9, 50.0}, 200);
    assert(partial.samples == 4);
    assert(partial.min == 39.9);
    assert(partial.median == 49.9);
    assert(partial.percentile95 == 50.0);
    assert(partial.max == 50.0);
}
