#ifndef ENDSTONE_SPARK_PROFILER_H
#define ENDSTONE_SPARK_PROFILER_H

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/profiler/profile_mode.h"
#include "core/recovery/recovery_writer.h"
#include "core/stats/ping_statistics.h"
#include "core/stats/statistics_service.h"
#include "core/stats/system_stats.h"
#include "native/alloc/allocation_sampler.h"
#include "native/sampler/heartbeat.h"
#include "native/sampler/sampler.h"
#include "proto/sampler_data.h"

namespace spark {

struct ProfilerTestAccess;

struct NativePluginSource {
    std::uintptr_t module_base = 0;
    std::string module_path;
    std::string source_id;
};

inline constexpr int kMaxSamplingIntervalMs = 1000;

// Parsed `/spark profiler start` options (spark's flag set).
struct ProfilerOptions {
    int interval_ms = 4;
    std::int32_t allocation_interval_bytes = kDefaultAllocationIntervalBytes;
    std::int64_t timeout_seconds = -1;
    std::int64_t only_ticks_over_ms = -1;  // -1 = disabled
    bool ignore_sleeping = false;
    bool regex = false;
    std::vector<std::string> threads;
    bool alloc = false;
    bool alloc_live_only = false;
    std::string comment;
    bool save_to_file = false;
    std::string creator_name = "Console";
    bool creator_is_player = false;
    ThreadGrouperMode thread_grouper = ThreadGrouperMode::ByPool;
    bool is_background = false;
    // Deterministic service-failure injection used only by the offline selftest.
    bool fail_allocation_aggregator_for_testing = false;
};

// Server facts needed only at export time (read from Endstone on the main thread).
struct ExportContext {
    std::string endstone_version;
    std::string minecraft_version;
    std::string bds_executable_sha256;
    std::string comment;  // overrides the start-time comment when non-empty
    std::int64_t player_count = -1;
    int online_mode = 0;  // 0 unknown, 1 offline, 2 online
    std::int64_t uptime_ms = 0;
    StatisticsSnapshot statistics;
    SystemStats system_stats;
    std::map<std::int32_t, WindowStats> window_stats;
    std::vector<PluginInfo> plugins;
    std::vector<NativePluginSource> native_plugin_sources;
    WorldInfo world;
    std::map<std::string, std::string> server_configurations;
    // Ping rolling average snapshot for profile metadata (may be empty).
    std::vector<int> ping_samples;
    // Network interface rolling average snapshots (may be empty).
    std::map<std::string, NetworkInterfaceSnapshot> net_snapshots;
    // Pre-serialized SocketChannelInfo proto for live viewer (empty for normal exports).
    std::string socket_channel_info_proto;
};

// Owns either the execution sampler or the platform allocation sampler and turns
// its call tree into a spark SamplerData payload.
class Profiler {
public:
    bool running() const { return running_.load(); }
    std::int64_t startTimeMs() const { return start_time_ms_; }
    std::int64_t autoEndTimeMs() const { return auto_end_time_ms_; }
    std::int64_t endTimeMs() const { return end_time_ms_; }
    const ProfilerOptions &options() const { return options_; }
    ProfileMode mode() const { return mode_; }
    std::uint64_t sampleCount() const;
    std::uint64_t sampledAllocationBytes() const;
    std::uint64_t observedAllocationBytes() const;
    std::uint64_t droppedSamples() const;
    std::uint64_t filteredAllocationSamples() const;
    std::uint64_t allocationThreadNameFailures() const;
    std::uint64_t freedAllocationSamples() const;
    std::uint64_t liveAllocationSamples() const;
    std::uint64_t liveAllocationBytes() const;
    bool backendFailure(std::string &error) const;
    const std::vector<AllocationHookCapability> &allocationHookCapabilities() const;
    std::size_t allocationHookTargetCount() const;

    // Returns false and sets `error` if sampling can't start.
    bool start(const ProfilerOptions &options, std::uint64_t main_tid, std::string &error);
    void onTick(double mspt_ms);

    // Two-phase stop: stopSampling() joins service threads on the main thread;
    // exportData() performs symbolication and serialization on a background thread
    // once sampling has stopped.
    bool stopSampling(std::string &error);
    void stopSampling();  // compatibility helper that discards the error
    void requestStop() { sampling_stop_requested_.store(true, std::memory_order_release); }
    std::string exportData(const ExportContext &ctx) const;

    // Export while the profiler is still running.
    std::string liveExport(const ExportContext &ctx);
    bool setCurrentThreadAllocationTrackingSuppressed(bool suppressed) noexcept;

    // Convenience (used by the self-test): stopSampling() + exportData().
    std::string stop(const ExportContext &ctx);
    bool cancel(std::string &error);
    void cancel();  // compatibility helper

    // Heartbeats from the execution sampler's service threads.
    const Heartbeat &samplerHeartbeat() const { return sampler_.samplerHeartbeat(); }
    const Heartbeat &aggregatorHeartbeat() const { return sampler_.aggregatorHeartbeat(); }

    // Sets the recovery journal directory before start().
    void setRecoveryDirectory(std::filesystem::path dir) { recovery_dir_ = std::move(dir); }

    // Deletes the recovery journal directory.  Called after a successful
    // export, a user cancel, or a clean shutdown so the next startup does not
    // mistake the journal for a crash to recover.  Safe to call when no
    // journal exists.
    void discardRecoveryJournal();

    void journalStallBegin(std::uint64_t detected_ns, std::uint64_t last_tick_ns);
    void journalStallEnd(std::uint64_t detected_ns, std::uint64_t recovered_ns);

    // Unconditionally closes the active backend and destroys native hook
    // trampolines. Must run before the plugin module is unloaded.
    bool shutdown(std::string &error);

private:
    friend struct ProfilerTestAccess;

    static void addNativePluginSources(ProfileMetadata &meta, const ExportContext &ctx,
                                       const std::vector<FrameKey> &keys,
                                       const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved);
    const CallTree &activeTree() const;
    const ModuleTable &activeModules() const;
    std::uint64_t activeNumberOfTicks() const;
    std::string exportData(const ExportContext &ctx, const AllocationSnapshot *allocation_snapshot) const;
    void stopRecoveryWriter();

    Sampler sampler_;
    AllocationSampler allocation_sampler_;
    ProfilerOptions options_;
    ProfileMode mode_ = ProfileMode::Execution;
    std::atomic<bool> running_{false};
    std::int64_t start_time_ms_ = 0;
    std::int64_t end_time_ms_ = 0;
    std::int64_t auto_end_time_ms_ = -1;
    std::int32_t interval_ = 4000;  // execution: microseconds; allocation: bytes
    std::filesystem::path recovery_dir_;
    mutable std::mutex recovery_mutex_;
    std::unique_ptr<RecoveryWriter> recovery_writer_;
    mutable std::mutex lifecycle_mutex_;
    std::atomic<bool> sampling_stop_requested_{false};
    std::function<void()> live_export_paused_hook_;
    std::function<void()> stop_requested_hook_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_PROFILER_H
