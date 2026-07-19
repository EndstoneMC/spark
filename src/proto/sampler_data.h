#ifndef ENDSTONE_SPARK_SAMPLER_DATA_H
#define ENDSTONE_SPARK_SAMPLER_DATA_H

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sampler/call_tree.h"
#include "sampler/profile_mode.h"
#include "sampler/symbolicate.h"
#include "sampler/types.h"
#include "stats/system_stats.h"

namespace spark {

struct ThreadTreeView {
    std::string_view name;
    const CallTree *tree = nullptr;
};

// Everything about the run that isn't the call tree itself.
struct ProfileMetadata {
    std::int64_t start_time_ms = 0;
    std::int64_t end_time_ms = 0;
    std::int32_t interval = 4000;  // execution: microseconds; allocation: bytes
    ProfileMode mode = ProfileMode::Execution;
    std::int32_t number_of_ticks = 0;
    std::string endstone_version;
    std::string minecraft_version;
    std::string engine_version;  // e.g. "endstone-spark 0.1.0"
    std::string comment;
    std::string creator_name = "Console";
    bool creator_is_player = false;
    std::string thread_name = "Server thread";
    bool all_threads = false;
    bool ticked = false;             // --only-ticks-over active
    std::int64_t tick_threshold_ms = 0;
    PlatformStats platform_stats;
    SystemStats system_stats;
    std::map<std::int32_t, WindowStats> window_stats;
    std::map<std::string, std::string> extra_platform_metadata;
    std::vector<PluginInfo> plugins;
    WorldInfo world;
};

// Collect every distinct frame key present in the tree (for batch symbolication).
std::vector<FrameKey> collectFrameKeys(const CallTree &tree);
std::vector<FrameKey> collectFrameKeys(const std::vector<ThreadTreeView> &threads);

// Serialize a spark `SamplerData` protobuf message (uncompressed bytes).
std::string buildSamplerData(const ProfileMetadata &meta, const CallTree &tree,
                             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved);
std::string buildSamplerData(const ProfileMetadata &meta, const std::vector<ThreadTreeView> &threads,
                             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved);

}  // namespace spark

#endif  // ENDSTONE_SPARK_SAMPLER_DATA_H
