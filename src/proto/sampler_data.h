#ifndef ENDSTONE_SPARK_SAMPLER_DATA_H
#define ENDSTONE_SPARK_SAMPLER_DATA_H

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/profiler/profile_mode.h"
#include "core/stats/statistics_service.h"
#include "core/stats/system_stats.h"
#include "native/sampler/call_tree.h"
#include "native/sampler/types.h"
#include "native/symbol/symbolicate.h"

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
    bool regex_threads = false;
    std::vector<std::int64_t> thread_ids;
    std::vector<std::string> thread_patterns;
    bool ticked = false;  // --only-ticks-over active
    std::int64_t tick_threshold_ms = 0;
    ThreadGrouperMode thread_grouper = ThreadGrouperMode::ByPool;
    PlatformStats platform_stats;
    SystemStats system_stats;
    StatisticsSnapshot statistics;
    std::map<std::int32_t, WindowStats> window_stats;
    std::map<std::string, std::string> extra_platform_metadata;
    std::map<std::string, std::string> server_configurations;
    std::vector<PluginInfo> plugins;
    std::map<std::string, std::string> class_sources;
    WorldInfo world;
    std::string socket_channel_info_proto;  // field 8: SocketChannelInfo (empty for non-live)
};

// Collect every distinct frame key present in the tree (for batch symbolication).
std::vector<FrameKey> collectFrameKeys(const CallTree &tree);
std::vector<FrameKey> collectFrameKeys(const std::vector<ThreadTreeView> &threads);

// Serialize a spark `SamplerData` protobuf message (uncompressed bytes).
std::string buildSamplerData(const ProfileMetadata &meta, const CallTree &tree,
                             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved);
std::string buildSamplerData(const ProfileMetadata &meta, const std::vector<ThreadTreeView> &threads,
                             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved);

// Data needed to build a spark `HealthData` protobuf message.
struct HealthData {
    std::string creator_name = "Console";
    bool creator_is_player = false;
    std::string endstone_version;
    std::string minecraft_version;
    PlatformStats platform_stats;
    SystemStats system_stats;
    StatisticsSnapshot statistics;
    std::int64_t generated_time_ms = 0;
    std::vector<PluginInfo> plugins;
    std::map<std::string, std::string> server_configurations;
    std::map<std::string, std::string> extra_platform_metadata;
    std::map<std::int32_t, WindowStats> window_stats;
};

// Serialize a spark `HealthData` protobuf message (uncompressed bytes).
std::string buildHealthData(const HealthData &data);

}  // namespace spark

#endif  // ENDSTONE_SPARK_SAMPLER_DATA_H
