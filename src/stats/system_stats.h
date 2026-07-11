#ifndef ENDSTONE_SPARK_SYSTEM_STATS_H
#define ENDSTONE_SPARK_SYSTEM_STATS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace spark {

// A loaded plugin, for the viewer's Plugins/Mods list (SamplerMetadata.sources).
struct PluginInfo {
    std::string name;
    std::string version;
    std::string author;
    std::string description;
};

// Per-dimension entity totals + an aggregate type breakdown, for the World view.
struct WorldEntry {
    std::string name;
    int total_entities = 0;
};

struct WorldInfo {
    bool present = false;
    int total_entities = 0;
    std::map<std::string, int> entity_counts;  // entity type -> count
    std::vector<WorldEntry> worlds;            // one per dimension
};

// Server-side statistics sourced from the Endstone API (on the main thread).
struct PlatformStats {
    bool present = false;
    double tps = 0.0;  // average tps (fanned out to last1m/5m/15m)
    int target_tps = 20;
    double mspt = 0.0;  // average mspt (mean/median)
    double mspt_max = 0.0;
    int max_ideal_mspt = 50;
    long player_count = -1;
    int online_mode = 0;  // 0 unknown, 1 offline, 2 online
    std::int64_t uptime_ms = 0;
    std::int64_t process_mem_bytes = 0;      // VmRSS, reported as "used" of the process pool
    std::int64_t process_virtual_bytes = 0;  // VmSize, reported as the pool's total (like /status)
};

// Host statistics gathered from /proc, statvfs and uname.
struct SystemStats {
    bool present = false;
    int cpu_threads = 0;
    std::string cpu_model;
    double cpu_process = 0.0;  // 0..1 of total capacity
    double cpu_system = 0.0;   // 0..1
    std::int64_t mem_used = 0, mem_total = 0;
    std::int64_t swap_used = 0, swap_total = 0;
    std::int64_t disk_used = 0, disk_total = 0;
    std::string os_arch, os_name, os_version;
    std::int64_t uptime_ms = 0;
};

// Per-time-window statistics for the viewer's timeline overlay.
struct WindowStats {
    int ticks = 0;
    double tps = 0.0;
    double mspt_median = 0.0;
    double mspt_max = 0.0;
};

// A point-in-time CPU-time reading; two of them yield a usage fraction.
struct CpuSnapshot {
    bool valid = false;
    unsigned long long process_ticks = 0;  // utime + stime
    unsigned long long system_busy = 0;
    unsigned long long system_total = 0;
    std::int64_t wall_ms = 0;
};

CpuSnapshot captureCpuSnapshot();

// Gather host stats; CPU usage is computed against `baseline`. `disk_path` selects
// the filesystem to report (e.g. the server working directory).
SystemStats gatherSystemStats(const CpuSnapshot &baseline, const std::string &disk_path);

std::int64_t processRssBytes();      // VmRSS
std::int64_t processVirtualBytes();  // VmSize

}  // namespace spark

#endif  // ENDSTONE_SPARK_SYSTEM_STATS_H
