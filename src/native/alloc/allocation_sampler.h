#ifndef ENDSTONE_SPARK_ALLOCATION_SAMPLER_H
#define ENDSTONE_SPARK_ALLOCATION_SAMPLER_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "native/sampler/call_tree.h"
#include "native/sampler/recovery_sink.h"
#include "native/sampler/sampler.h"
#include "native/sampler/types.h"

namespace spark {

#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
namespace test {
struct AllocationLifecycleTestAccess;
struct AllocationDiagnosticsTestAccess;
}  // namespace test
#endif

inline constexpr std::int32_t kDefaultAllocationIntervalBytes = 524287;  // spark's default: ~512 KiB
inline constexpr std::int32_t kMaxAllocationIntervalBytes = 0x7fffffff;

struct AllocationSamplerConfig {
    std::int32_t interval_bytes = kDefaultAllocationIntervalBytes;
    std::uint64_t session_seed = 0;
    std::int64_t only_ticks_over_ms = 0;
    // Allocation-origin names are matched by the aggregator. Empty patterns with
    // all_threads=true preserve the default process-wide behavior.
    bool all_threads = true;
    bool regex_threads = false;
    std::vector<std::string> thread_patterns;
    bool live_only = false;
    // Count successful requested allocation bytes only. Existing allocator hooks
    // and recursion/alias suppression remain active, but stack capture, lifecycle
    // tracking, event queues and the aggregator are bypassed.
    bool count_only = false;
    // Deterministic fault injection used only by the offline selftest.
    bool fail_aggregator_for_testing = false;
    std::uint32_t aggregator_delay_ms_for_testing = 0;
    // Deterministic per-event aggregator cost, used by the bounded-drain test.
    std::uint32_t aggregator_per_event_delay_us_for_testing = 0;
    // Deterministic per-record cost for the retained live-profile walk.
    std::uint32_t live_finalize_per_record_delay_us_for_testing = 0;
    // Keeps the consumer side of the allocation event queue parked until a
    // producer has observed a bounded queue drop (or the session is stopped).
    // This makes queue-capacity pressure tests independent of host speed.
    bool hold_aggregator_until_event_drop_for_testing = false;
    std::uint32_t thread_state_limit_for_testing = 0;
    bool force_live_lock_contention_for_testing = false;
    std::atomic<std::uint64_t> *observed_thread_identities_for_testing = nullptr;
};

enum class AllocationHookStatus {
    Active,
    Alias,
    Missing,
    PrepareFailed,
};

inline const char *allocationHookStatusName(AllocationHookStatus status) noexcept
{
    switch (status) {
    case AllocationHookStatus::Active:
        return "active";
    case AllocationHookStatus::Alias:
        return "alias";
    case AllocationHookStatus::Missing:
        return "missing";
    case AllocationHookStatus::PrepareFailed:
        return "prepare-failed";
    }
    return "unknown";
}

struct AllocationHookCapability {
    std::string name;
    AllocationHookStatus status = AllocationHookStatus::Missing;
    std::string detail;
};

struct AllocationSnapshot {
    CallTree tree;
    std::map<std::uint64_t, ThreadCallTree> thread_trees;
    ModuleTable modules{};
    std::uint64_t number_of_ticks = 0;
    std::uint64_t sample_count = 0;
    std::uint64_t sampled_bytes = 0;
    std::uint64_t retained_average_age_ms = 0;
    std::uint64_t retained_maximum_age_ms = 0;
};

enum class AllocationAccountingState {
    NotStarted,
    Active,
    Incomplete,
    Complete,
    Failed,
    NotApplicable,
};

inline const char *allocationAccountingStateName(AllocationAccountingState state) noexcept
{
    switch (state) {
    case AllocationAccountingState::NotStarted:
        return "NotStarted";
    case AllocationAccountingState::Active:
        return "Active";
    case AllocationAccountingState::Incomplete:
        return "Incomplete";
    case AllocationAccountingState::Complete:
        return "Complete";
    case AllocationAccountingState::Failed:
        return "Failed";
    case AllocationAccountingState::NotApplicable:
        return "NotApplicable";
    }
    return "Unknown";
}

// Session diagnostics are backed by bounded atomics in the native backends.
// Values may be in flight while a session is active, but are exact after
// producer quiescence and terminal drain.
struct AllocationDiagnostics {
    bool supported = false;
    AllocationAccountingState accounting_state = AllocationAccountingState::NotStarted;
    std::uint64_t live_index_capacity = 0;
    std::uint64_t live_record_capacity = 0;

    std::uint64_t drain_truncated = 0;
    std::uint64_t drain_truncated_allocation_events = 0;
    std::uint64_t drain_truncated_thread_observation_events = 0;
    std::uint64_t drain_truncated_tick_events = 0;
    std::uint64_t retained_allocations_skipped = 0;
    std::uint64_t record_pool_acquisition_failures = 0;
    std::uint64_t insertion_contention_failures = 0;
    std::uint64_t exhausted_insertion_probe_failures = 0;
    std::uint64_t detach_contention_attempts = 0;

    std::uint64_t processed_allocation_events = 0;
    std::uint64_t processed_thread_observation_events = 0;
    std::uint64_t processed_tick_events = 0;
    std::uint64_t discarded_allocation_events = 0;
    std::uint64_t discarded_thread_observation_events = 0;
    std::uint64_t discarded_tick_events = 0;

    std::uint64_t consumer_lifetime_elapsed_ns = 0;
    std::uint64_t active_drain_elapsed_ns = 0;
    std::uint64_t caller_final_drain_elapsed_ns = 0;
    std::uint64_t caller_final_drain_allocation_events = 0;
    std::uint64_t caller_final_drain_thread_observation_events = 0;
    std::uint64_t caller_final_drain_tick_events = 0;

    bool aggregator_cpu_supported = false;
    bool aggregator_cpu_valid = false;
    bool aggregator_cpu_read_failure = false;
    std::uint64_t aggregator_cpu_time_ns = 0;

    bool module_cache_supported = false;
    std::uint64_t module_cache_hits = 0;
    std::uint64_t module_cache_misses = 0;
    std::uint64_t module_cache_insertion_refusals = 0;
    std::uint64_t module_cache_size = 0;
    std::uint64_t module_cache_capacity = 0;
};

using AllocationDiagnosticsSnapshot = AllocationDiagnostics;

// Native allocation sampler. Tree weights are allocation bytes; lifecycle tracking
// spans all covered threads so realloc/free may occur on a different thread.
class AllocationSampler {
public:
    AllocationSampler();
    ~AllocationSampler();

    AllocationSampler(const AllocationSampler &) = delete;
    AllocationSampler &operator=(const AllocationSampler &) = delete;

    bool start(const AllocationSamplerConfig &config, std::string &error);

    // Stops the current session and drains its aggregator. Entry hooks remain
    // installed as disabled pass-throughs until shutdown(), avoiding allocator
    // prologue patching on every start/stop cycle.
    bool stop(std::string &error);

    // Stops tracking and the producer without draining, finalizing, uninstalling hooks, or joining.
    void requestStop() noexcept;

    // Final lifecycle cleanup. This is safe to call even when no session is
    // running and must be called before the plugin module can be unloaded.
    bool shutdown(std::string &error);

    void onTick(double mspt_ms);

    bool snapshot(AllocationSnapshot &snapshot, std::string &error);
    AllocationDiagnostics diagnostics() const;
    AllocationDiagnosticsSnapshot allocationDiagnostics() const;
    bool setCurrentThreadTrackingSuppressed(bool suppressed) noexcept;

    // Sets the recovery sink for crash-safe journaling.  Must be called
    // before start().  All RecoverySink methods are non-blocking.
    void setRecoverySink(RecoverySink *sink);

    const CallTree &tree() const;
    const std::map<std::uint64_t, ThreadCallTree> &threadTrees() const;
    const ModuleTable &modules() const;
    const std::map<std::int32_t, WindowTickStats> &windowTicks() const;

    std::uint64_t numberOfTicks() const;
    std::uint64_t hookCalls() const;
    std::uint64_t successfulAllocationCalls() const;
    std::uint64_t sampleCount() const;
    std::uint64_t samplingPoints() const;
    std::uint64_t sampledBytes() const;
    std::uint64_t filteredSamples() const;
    std::uint64_t threadNameFailures() const;
    std::uint64_t threadIdentityCacheDrops() const;
    std::uint64_t observedBytes() const;
    std::uint64_t droppedSamples() const;
    std::uint64_t droppedEvents() const;
    std::uint64_t droppedTickEvents() const;
    static std::uint64_t tickEventCapacity();
    std::uint64_t enqueuedSamples() const;
    std::uint64_t eventQueueHighWaterMark() const;
    static std::uint64_t eventQueueCapacity();
    std::uint64_t freedSamples() const;
    std::uint64_t freedBytes() const;
    std::uint64_t liveSamples() const;
    std::uint64_t liveBytes() const;
    std::uint64_t peakLiveSamples() const;
    static std::uint64_t liveIndexCapacity();
    static std::uint64_t liveRecordCapacity();
    static std::uint64_t moduleCacheCapacity();
    std::uint64_t sampledThreadCount() const;
    static std::uint64_t threadRootCapacity();
    std::uint64_t overflowThreadCount() const;
    std::uint64_t threadStateDrops() const;
    std::uint64_t hookedModuleCount() const;
    std::uint64_t skippedModuleCount() const;
    std::uint64_t failedModuleCount() const;
    std::uint64_t moduleRegistryCount() const;
    static std::uint64_t moduleRegistryCapacity();
    static std::uint64_t profileNodeCapacity();
    static std::uint64_t profileTimeEntryCapacity();
    std::uint64_t profileStorageSampleDrops() const;
    bool profileStorageExhausted() const;
    static std::uint64_t pendingSampleCapacity();
    std::uint64_t pendingSampleDrops() const;
    std::uint64_t pendingCapacityDrops() const;
    std::uint64_t pendingStaleDrops() const;
    std::uint64_t terminalInFlightTickSamplesDiscarded() const;
    // Deprecated compatibility alias for terminalInFlightTickSamplesDiscarded().
    std::uint64_t pendingFinalDrops() const;
    std::uint64_t moduleOverflowFrames() const;
    std::uint64_t retainedHistoryWindows() const;
    std::uint64_t historySamplesPruned() const;
    std::uint64_t historyBytesPruned() const;
    bool historyTruncated() const;
    bool dataIncomplete() const;
    std::uint64_t averageLifetimeMs() const;
    std::uint64_t maximumLifetimeMs() const;
    std::uint64_t lifecycleDropped() const;
    std::uint64_t contentionDropped() const;
    std::uint64_t drainTruncated() const;
    bool stopWaitTimedOut() const;
    bool aggregatorMayBeAlive() const;
    bool backendCleanupPending() const;
    std::uint64_t retainedAverageAgeMs() const;
    std::uint64_t retainedMaximumAgeMs() const;
    bool running() const;
    bool hooksInstalled() const;
    bool failure(std::string &error) const;
    static const char *backendId() noexcept;
    static const char *backendName() noexcept;
    const char *resolvedBackendName() const noexcept;
    const std::vector<AllocationHookCapability> &hookCapabilities() const;
    std::size_t hookTargetCount() const;

private:
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
    friend struct test::AllocationLifecycleTestAccess;
    friend struct test::AllocationDiagnosticsTestAccess;
#endif
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_ALLOCATION_SAMPLER_H
