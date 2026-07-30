#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "alloc/allocation_sampler.h"
#include "sampler/thread_info.h"

namespace {

using Clock = std::chrono::steady_clock;

void allocationWork(std::size_t operations)
{
    for (std::size_t i = 0; i < operations; ++i) {
        void *pointer = std::malloc(64 + (i & 63));
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] =
                static_cast<unsigned char>(i);
            std::free(pointer);
        }
    }
}

double measure(std::size_t threads, std::size_t operations_per_thread)
{
    const auto start = Clock::now();
    if (threads == 1) {
        allocationWork(operations_per_thread);
    }
    else {
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers.emplace_back(allocationWork, operations_per_thread);
        }
        for (std::thread &worker : workers) {
            worker.join();
        }
    }
    const auto elapsed = Clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count();
}

double runTrials(std::size_t threads, std::size_t operations_per_thread)
{
    std::vector<double> trials;
    trials.reserve(5);
    for (int trial = 0; trial < 5; ++trial) {
        trials.push_back(measure(threads, operations_per_thread));
    }
    std::sort(trials.begin(), trials.end());
    return trials[trials.size() / 2];
}

void printResult(const char *name, std::size_t threads, std::int32_t interval,
                 bool live_only, std::size_t operations_per_thread,
                 double elapsed_ns, std::uint64_t samples,
                 std::uint64_t dropped)
{
    const auto operations = static_cast<double>(threads * operations_per_thread);
    std::printf("%s,%zu,%d,%d,%zu,%.0f,%.2f,%llu,%llu\n", name, threads,
                interval, live_only ? 1 : 0, threads * operations_per_thread,
                elapsed_ns, elapsed_ns / operations,
                static_cast<unsigned long long>(samples),
                static_cast<unsigned long long>(dropped));
}

bool runProfiledCase(spark::AllocationSampler &sampler, const char *name,
                     std::size_t threads, std::int32_t interval, bool live_only,
                     std::size_t operations_per_thread, bool saturated)
{
    spark::AllocationSamplerConfig config;
    config.interval_bytes = interval;
    config.session_seed = spark::currentNativeThreadId();
    config.live_only = live_only;
#if defined(SPARK_ALLOCATION_BENCHMARK_CURRENT)
    config.aggregator_delay_ms_for_testing = saturated ? 1000 : 0;
#else
    (void)saturated;
#endif

    std::string error;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "%s: start failed: %s\n", name, error.c_str());
        return false;
    }
    const double elapsed = runTrials(threads, operations_per_thread);
    if (!sampler.stop(error)) {
        std::fprintf(stderr, "%s: stop failed: %s\n", name, error.c_str());
        return false;
    }
#if defined(SPARK_ALLOCATION_BENCHMARK_CURRENT)
    const std::uint64_t dropped = sampler.droppedEvents();
#else
    const std::uint64_t dropped = sampler.droppedSamples();
#endif
    printResult(name, threads, interval, live_only, operations_per_thread,
                elapsed, sampler.sampleCount(), dropped);
    return true;
}

}  // namespace

int main()
{
    constexpr std::size_t kOperations = 200000;
    constexpr std::size_t kPressureOperations = 16384;

    std::printf("case,threads,interval,live_only,operations_per_trial,median_ns,"
                "ns_per_op,samples_all_trials,dropped_all_trials\n");
    printResult("unprofiled", 1, 0, false, kOperations,
                runTrials(1, kOperations), 0, 0);
    printResult("unprofiled", 4, 0, false, kOperations,
                runTrials(4, kOperations), 0, 0);

    spark::AllocationSampler sampler;
    if (!runProfiledCase(sampler, "normal-default", 1,
                         spark::kDefaultAllocationIntervalBytes, false,
                         kOperations, false)) {
        std::string ignored;
        sampler.shutdown(ignored);
        return 1;
    }
    printResult("disabled-hooks", 1, 0, false, kOperations,
                runTrials(1, kOperations), 0, 0);
    printResult("disabled-hooks", 4, 0, false, kOperations,
                runTrials(4, kOperations), 0, 0);

    if (!runProfiledCase(sampler, "normal-default", 4,
                         spark::kDefaultAllocationIntervalBytes, false,
                         kOperations, false) ||
        !runProfiledCase(sampler, "normal-4k", 1, 4096, false, kOperations,
                         false) ||
        !runProfiledCase(sampler, "normal-4k", 4, 4096, false, kOperations,
                         false) ||
        !runProfiledCase(sampler, "live-4k", 1, 4096, true, kOperations,
                         false) ||
        !runProfiledCase(sampler, "live-4k", 4, 4096, true, kOperations,
                         false) ||
        !runProfiledCase(sampler, "saturated", 4, 1, false,
                         kPressureOperations, true)) {
        std::string ignored;
        sampler.shutdown(ignored);
        return 1;
    }

    std::string error;
    if (!sampler.shutdown(error)) {
        std::fprintf(stderr, "shutdown failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
