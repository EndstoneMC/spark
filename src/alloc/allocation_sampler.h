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
    std::uint64_t target_tid = 0;
    std::int64_t only_ticks_over_ms = 0;
    bool live_only = false;
    // Deterministic fault injection used only by the offline selftest.
    bool fail_aggregator_for_testing = false;
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

// Windows native allocation sampler. The backend hooks public UCRT allocation
// entry points and samples allocation stacks by requested bytes. Tree weights
// are estimated allocation bytes, not elapsed time. The current backend remains
// intentionally focused on allocations originating on the selected server
// thread. Sampled allocations are followed through realloc/free on any thread.
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
    const ModuleTable &modules() const;
    const std::map<std::int32_t, WindowTickStats> &windowTicks() const;

    std::uint64_t numberOfTicks() const;
    std::uint64_t sampleCount() const;
    std::uint64_t sampledBytes() const;
    std::uint64_t observedBytes() const;
    std::uint64_t droppedSamples() const;
    std::uint64_t freedSamples() const;
    std::uint64_t freedBytes() const;
    std::uint64_t liveSamples() const;
    std::uint64_t liveBytes() const;
    std::uint64_t averageLifetimeMs() const;
    std::uint64_t maximumLifetimeMs() const;
    std::uint64_t lifecycleDropped() const;
    std::uint64_t retainedAverageAgeMs() const;
    std::uint64_t retainedMaximumAgeMs() const;
    bool running() const;
    bool hooksInstalled() const;
    bool failure(std::string &error) const;
    const std::vector<AllocationHookCapability> &hookCapabilities() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_ALLOCATION_SAMPLER_H
