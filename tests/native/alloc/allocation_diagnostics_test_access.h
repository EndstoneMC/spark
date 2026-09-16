#ifndef ENDSTONE_SPARK_ALLOCATION_DIAGNOSTICS_TEST_ACCESS_H
#define ENDSTONE_SPARK_ALLOCATION_DIAGNOSTICS_TEST_ACCESS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "native/alloc/allocation_sampler.h"

namespace spark::test {

#if defined(_WIN32) && defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
struct AllocationMainImageState {
    std::uintptr_t base = 0;
    std::uintptr_t size = 0;
    std::uint64_t discoveries = 0;
    std::uint64_t fallback_queries = 0;
};

struct AllocationResolvedFrame {
    std::uint64_t module = 0;
    std::uint64_t rva = 0;
    std::uint64_t raw_address = 0;
    std::string path;

    bool operator==(const AllocationResolvedFrame &) const = default;
};

struct AllocationLiveRecordState {
    bool found = false;
    std::uint64_t allocation_id = 0;
    std::uint64_t requested_bytes = 0;
    std::uint64_t weight_bytes = 0;
};
#endif

struct AllocationEventProcessingGate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> timed_out{false};
};

struct AllocationFixtureWorkerGate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> timed_out{false};
    std::mutex seed_mutex;
    std::uint64_t timeout_ms = 5000;
};

struct AllocationFixtureSeedCounts {
    std::uint64_t allocation_events = 0;
    std::uint64_t thread_observation_events = 0;
    std::uint64_t tick_events = 0;
};

struct AllocationFixtureLockGate {
    std::atomic<bool> ready{false};
    std::atomic<bool> release{false};
    std::atomic<bool> timed_out{false};
};

struct AllocationFixtureLiveRecord {
    void *pointer = nullptr;
    void *opaque = nullptr;
};

struct AllocationFixtureCpuWorkControl {
    const std::uint64_t target_cpu_ns = 60'000'000;
    const std::uint32_t sleep_ms;
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> cancelled{false};
    std::atomic<std::uint64_t> checksum{0};
    std::atomic<std::uint64_t> iterations{0};
    std::atomic<bool> request_cancel{false};

    explicit AllocationFixtureCpuWorkControl(std::uint32_t sleep) : sleep_ms(sleep) {}
};

struct AllocationDiagnosticsTestAccess {
#if defined(_WIN32) && defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
    static bool mainImageRangeContains(std::uintptr_t base, std::uintptr_t size, std::uintptr_t address) noexcept;
    static bool mainImageState(AllocationSampler &, AllocationMainImageState &) noexcept;
    static bool forceMainImageDiscoveryFailure(AllocationSampler &, bool) noexcept;
    static bool resolveFrame(AllocationSampler &, std::uintptr_t address, bool force_fallback,
                             AllocationResolvedFrame &) noexcept;
    static bool liveRecordState(AllocationSampler &, void *, AllocationLiveRecordState &) noexcept;
#endif
    static bool configureFixture(AllocationSampler &, bool no_hooks, bool no_worker,
                                 AllocationFixtureWorkerGate *) noexcept;
    static bool releaseFixture(AllocationSampler &) noexcept;
    static bool fixtureStorageReady(const AllocationSampler &) noexcept;
    static bool fixtureWorkerPresent(const AllocationSampler &) noexcept;
    static bool fixtureHooksPresent(const AllocationSampler &) noexcept;
    static bool fixtureAggregatorRunning(const AllocationSampler &) noexcept;
    static bool seedFixtureQueues(AllocationSampler &, AllocationFixtureSeedCounts &) noexcept;
    static bool seedFixtureQueues(AllocationSampler &, AllocationFixtureSeedCounts &,
                                  const AllocationFixtureSeedCounts &) noexcept;
    static bool recordFixtureAllocation(AllocationSampler &, void *, std::uint64_t) noexcept;
    static bool configureFixtureCpuWork(AllocationSampler &, AllocationFixtureCpuWorkControl &) noexcept;
    static void forceAggregatorCpuReadFailure(AllocationSampler &, bool) noexcept;
    static void forceAggregatorCpuZero(AllocationSampler &, bool) noexcept;
    static void armEventProcessingGate(AllocationSampler &, AllocationEventProcessingGate &) noexcept;
    static void disarmEventProcessingGate(AllocationSampler &) noexcept;
    static void forceProcessEventFailure(AllocationSampler &, bool) noexcept;
    static void forceDrainDeadline(AllocationSampler &, bool) noexcept;
    static void forceRetainedWalkBudget(AllocationSampler &, bool) noexcept;
    static std::uint64_t retainedWalkVisits(const AllocationSampler &) noexcept;
    static bool drainFixtureAggregatorContext(AllocationSampler &) noexcept;
    static bool seedFixtureLiveAllocations(AllocationSampler &, void *const *, std::size_t) noexcept;
    static bool prepareFixtureLiveRecord(AllocationSampler &, void *, std::uint64_t,
                                         AllocationFixtureLiveRecord &) noexcept;
    static bool holdInsertionShardLock(AllocationSampler &, void *, AllocationFixtureLockGate &) noexcept;
    static bool holdDetachShardLock(AllocationSampler &, void *, AllocationFixtureLockGate &) noexcept;
    static bool insertFixtureLiveRecord(AllocationSampler &, AllocationFixtureLiveRecord &, bool) noexcept;
    static bool detachFixtureLiveRecord(AllocationSampler &, AllocationFixtureLiveRecord &) noexcept;
    static void retireFixtureLiveRecord(AllocationSampler &, AllocationFixtureLiveRecord &) noexcept;
    static void releaseFixtureLiveRecord(AllocationSampler &, AllocationFixtureLiveRecord &) noexcept;
    static bool fileTimeToNanoseconds(std::uint32_t high, std::uint32_t low, std::uint64_t &value) noexcept;
    static void seedModuleCache(AllocationSampler &, std::size_t entries) noexcept;
    static bool resolveFrameOnce(AllocationSampler &) noexcept;
    static bool resolveFrameTwice(AllocationSampler &) noexcept;
    static bool resolveMissingFrame(AllocationSampler &) noexcept;
    static bool exerciseRecordPoolEmpty(AllocationSampler &, void *) noexcept;
    static bool exerciseInsertionProbeExhaustion(AllocationSampler &, void *, std::size_t) noexcept;
};

}  // namespace spark::test

#endif  // ENDSTONE_SPARK_ALLOCATION_DIAGNOSTICS_TEST_ACCESS_H
