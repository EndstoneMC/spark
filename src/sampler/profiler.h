#ifndef ENDSTONE_SPARK_PROFILER_H
#define ENDSTONE_SPARK_PROFILER_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "sampler/sampler.h"
#include "stats/system_stats.h"

namespace spark {

// Parsed `/spark profiler start` options (spark's flag set).
struct ProfilerOptions {
    int interval_ms = 4;
    long timeout_seconds = -1;
    long only_ticks_over_ms = -1;  // -1 = disabled
    bool ignore_sleeping = true;
    bool regex = false;
    std::vector<std::string> threads;
    bool combine_all = false;
    bool not_combined = false;
    bool alloc = false;             // accepted, unsupported (no allocation engine)
    bool force_java_sampler = false;  // accepted, unsupported (no java engine)
    std::string comment;
    bool save_to_file = false;
    std::string creator_name = "Console";
    bool creator_is_player = false;
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

// Owns a Sampler and turns it into a spark SamplerData payload. This is the object
// the plugin drives; it has no Endstone dependency.
class Profiler {
public:
    bool running() const
    {
        return running_.load();
    }
    long startTimeMs() const
    {
        return start_time_ms_;
    }
    long autoEndTimeMs() const
    {
        return auto_end_time_ms_;
    }
    const ProfilerOptions &options() const
    {
        return options_;
    }
    std::uint64_t sampleCount() const
    {
        return sampler_.sampleCount();
    }

    // Returns false and sets `error` if sampling can't start.
    bool start(const ProfilerOptions &options, std::uint64_t main_tid, std::string &error);
    void onTick(double mspt_ms);

    // Two-phase stop: stopSampling() is fast (joins threads) and runs on the main
    // thread; exportData() does the slow symbolication + serialization and is safe
    // to run on a background thread once sampling has stopped.
    void stopSampling();
    std::string exportData(const ExportContext &ctx) const;

    // Convenience (used by the self-test): stopSampling() + exportData().
    std::string stop(const ExportContext &ctx);
    void cancel();

private:
    Sampler sampler_;
    ProfilerOptions options_;
    std::atomic<bool> running_{false};
    long start_time_ms_ = 0;
    long auto_end_time_ms_ = -1;
    int interval_us_ = 4000;
    CpuSnapshot cpu_baseline_{};
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_PROFILER_H
