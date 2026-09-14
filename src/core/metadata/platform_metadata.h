#ifndef ENDSTONE_SPARK_PLATFORM_METADATA_H
#define ENDSTONE_SPARK_PLATFORM_METADATA_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace spark {

struct NativePluginSource {
    std::uintptr_t module_base = 0;
    std::string module_path;
    std::string source_id;
};

struct PluginInfo {
    std::string name;
    std::string version;
    std::string author;
    std::string description;
};

struct WorldChunk {
    int x = 0;
    int z = 0;
    int total_entities = 0;
    std::map<std::string, int> entity_counts;
};

struct WorldRegion {
    int total_entities = 0;
    std::vector<WorldChunk> chunks;
};

struct WorldEntry {
    std::string name;
    int total_entities = 0;
    std::vector<WorldRegion> regions;
};

struct GameRuleInfo {
    std::string name;
    std::optional<std::string> default_value;
    std::map<std::string, std::string> world_values;
};

struct DataPackInfo {
    std::string name;
    std::string description;
    std::string source;
    bool builtin = false;
};

struct WorldInfo {
    bool present = false;
    int total_entities = 0;
    std::map<std::string, int> entity_counts;  // entity type -> count
    std::vector<WorldEntry> worlds;
    std::vector<GameRuleInfo> game_rules;
    std::vector<DataPackInfo> data_packs;
};

struct ServerMetadata {
    std::string endstone_version;
    std::string minecraft_version;
    std::string bds_executable_sha256;
    std::int64_t player_count = -1;
    int online_mode = 0;
    std::int64_t uptime_ms = 0;
    std::vector<PluginInfo> plugins;
    std::map<std::string, std::string> server_configurations;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_PLATFORM_METADATA_H
