#ifndef ENDSTONE_SPARK_ALLOCATION_SAMPLER_H
#define ENDSTONE_SPARK_ALLOCATION_SAMPLER_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sampler/call_tree.h"
#include "sampler/sampler.h"
#include "sampler/types.h"

namespace spark {

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
    // Deterministic fault injection used only by the offline selftest.
    bool fail_aggregator_for_testing = false;
    std::uint32_t aggregator_delay_ms_for_testing = 0;
    std::uint32_t thread_state_limit_for_testing = 0;
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

// Native allocation sampler. Tree weights are estimated allocation bytes, not
// elapsed time. Each process thread is sampled independently. Thread names are
// matched by the aggregator, while lifecycle tracking continues across every
// covered thread so realloc/free may occur on a different thread.
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

    // Final lifecycle cleanup. This is safe to call even when no session is
    // running and must be called before the plugin module can be unloaded.
    bool shutdown(std::string &error);

    void onTick(double mspt_ms);

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
    std::uint64_t tickEventCapacity() const;
    std::uint64_t enqueuedSamples() const;
    std::uint64_t eventQueueHighWaterMark() const;
    std::uint64_t eventQueueCapacity() const;
    std::uint64_t freedSamples() const;
    std::uint64_t freedBytes() const;
    std::uint64_t liveSamples() const;
    std::uint64_t liveBytes() const;
    std::uint64_t peakLiveSamples() const;
    std::uint64_t liveIndexCapacity() const;
    std::uint64_t sampledThreadCount() const;
    std::uint64_t threadRootCapacity() const;
    std::uint64_t overflowThreadCount() const;
    std::uint64_t threadStateDrops() const;
    std::uint64_t hookedModuleCount() const;
    std::uint64_t skippedModuleCount() const;
    std::uint64_t failedModuleCount() const;
    std::uint64_t moduleRegistryCount() const;
    std::uint64_t moduleRegistryCapacity() const;
    std::uint64_t profileNodeCapacity() const;
    bool dataIncomplete() const;
    std::uint64_t averageLifetimeMs() const;
    std::uint64_t maximumLifetimeMs() const;
    std::uint64_t lifecycleDropped() const;
    std::uint64_t retainedAverageAgeMs() const;
    std::uint64_t retainedMaximumAgeMs() const;
    bool running() const;
    bool hooksInstalled() const;
    bool failure(std::string &error) const;
    const std::vector<AllocationHookCapability> &hookCapabilities() const;
    std::size_t hookTargetCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_ALLOCATION_SAMPLER_H
