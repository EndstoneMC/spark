#ifndef ENDSTONE_SPARK_PROFILER_H
#define ENDSTONE_SPARK_PROFILER_H

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "alloc/allocation_sampler.h"
#include "sampler/profile_mode.h"
#include "sampler/sampler.h"
#include "stats/system_stats.h"

namespace spark {

inline constexpr int kMaxSamplingIntervalMs = 1000;

// Parsed `/spark profiler start` options (spark's flag set).
struct ProfilerOptions {
    int interval_ms = 4;
    std::int32_t allocation_interval_bytes = kDefaultAllocationIntervalBytes;
    long timeout_seconds = -1;
    long only_ticks_over_ms = -1;  // -1 = disabled
    bool ignore_sleeping = true;
    bool regex = false;
    std::vector<std::string> threads;
    bool combine_all = false;
    bool not_combined = false;
    bool alloc = false;
    bool alloc_live_only = false;
    bool force_java_sampler = false;  // accepted, unsupported (no Java engine)
    std::string comment;
    bool save_to_file = false;
    std::string creator_name = "Console";
    bool creator_is_player = false;
    // Deterministic service-failure injection used only by the offline selftest.
    bool fail_allocation_aggregator_for_testing = false;
};

// Server facts needed only at export time (read from Endstone on the main thread).
struct ExportContext {
    std::string endstone_version;
    std::string minecraft_version;
    std::string comment;  // overrides the start-time comment when non-empty
    double tps = 0.0;
    double mspt = 0.0;
    double mspt_max = 0.0;
    long player_count = -1;
    int online_mode = 0;  // 0 unknown, 1 offline, 2 online
    std::int64_t uptime_ms = 0;
    std::vector<PluginInfo> plugins;
    WorldInfo world;
};

// Owns either the execution sampler or the Windows native allocation sampler and
// turns its call tree into a spark SamplerData payload.
class Profiler {
public:
    bool running() const
    {
        return running_.load();
    }
    std::int64_t startTimeMs() const
    {
        return start_time_ms_;
    }
    std::int64_t autoEndTimeMs() const
    {
        return auto_end_time_ms_;
    }
    const ProfilerOptions &options() const
    {
        return options_;
    }
    ProfileMode mode() const
    {
        return mode_;
    }
    std::uint64_t sampleCount() const;
    std::uint64_t sampledAllocationBytes() const;
    std::uint64_t observedAllocationBytes() const;
    std::uint64_t droppedSamples() const;
    std::uint64_t freedAllocationSamples() const;
    std::uint64_t liveAllocationSamples() const;
    std::uint64_t liveAllocationBytes() const;
    bool backendFailure(std::string &error) const;
    const std::vector<AllocationHookCapability> &allocationHookCapabilities() const;

    // Returns false and sets `error` if sampling can't start.
    bool start(const ProfilerOptions &options, std::uint64_t main_tid, std::string &error);
    void onTick(double mspt_ms);

    // Two-phase stop: stopSampling() is fast (joins threads) and runs on the main
    // thread; exportData() does the slow symbolication + serialization and is safe
    // to run on a background thread once sampling has stopped.
    bool stopSampling(std::string &error);
    void stopSampling();  // compatibility helper for execution-only self-tests
    std::string exportData(const ExportContext &ctx) const;

    // Convenience (used by the self-test): stopSampling() + exportData().
    std::string stop(const ExportContext &ctx);
    bool cancel(std::string &error);
    void cancel();  // compatibility helper

    // Unconditionally closes the active backend and destroys native hook
    // trampolines. Must run before the plugin module is unloaded.
    bool shutdown(std::string &error);

private:
    const CallTree &activeTree() const;
    const ModuleTable &activeModules() const;
    const std::map<std::int32_t, WindowTickStats> &activeWindowTicks() const;
    std::uint64_t activeNumberOfTicks() const;

    Sampler sampler_;
    AllocationSampler allocation_sampler_;
    ProfilerOptions options_;
    ProfileMode mode_ = ProfileMode::Execution;
    std::atomic<bool> running_{false};
    std::int64_t start_time_ms_ = 0;
    std::int64_t auto_end_time_ms_ = -1;
    std::int32_t interval_ = 4000;  // execution: microseconds; allocation: bytes
    CpuSnapshot cpu_baseline_{};
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_PROFILER_H
