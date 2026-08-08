#ifndef ENDSTONE_SPARK_SYSTEM_STATS_H
#define ENDSTONE_SPARK_SYSTEM_STATS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/stats/network_monitor.h"

namespace spark {

// A loaded plugin, for the viewer's Plugins/Mods list (SamplerMetadata.sources).
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

struct WorldInfo {
    bool present = false;
    int total_entities = 0;
    std::map<std::string, int> entity_counts;  // entity type -> count
    std::vector<WorldEntry> worlds;
};

// Server-side statistics sourced from the Endstone API (on the main thread).
struct PlatformStats {
    bool present = false;
    int target_tps = 20;
    int max_ideal_mspt = 50;
    std::int64_t player_count = -1;
    int online_mode = 0;  // 0 unknown, 1 offline, 2 online
    std::int64_t uptime_ms = 0;
    bool process_mem_present = false;
    std::int64_t process_mem_bytes = 0;  // resident working set / VmRSS
    bool process_virtual_present = false;
    std::int64_t process_virtual_bytes = 0;  // reserved or committed virtual address space / VmSize
    // Ping rolling average (PlatformStatistics.Ping field 6).
    bool ping_present = false;
    double ping_mean = 0.0;
    double ping_max = 0.0;
    double ping_min = 0.0;
    double ping_median = 0.0;
    double ping_p95 = 0.0;
};

// Point-in-time process resources. Availability is explicit so a failed OS query
// is never presented as a real zero.
struct ProcessStats {
    bool rss_present = false;
    std::int64_t rss_bytes = 0;
    bool virtual_present = false;
    std::int64_t virtual_bytes = 0;
    bool threads_present = false;
    int threads = 0;
};

// Point-in-time host resources gathered through the native platform APIs.
// CPU usage is deliberately absent: rolling CPU values come only from
// StatisticsService.
struct SystemStats {
    bool present = false;
    bool cpu_present = false;
    int cpu_threads = 0;
    std::string cpu_model;
    bool memory_present = false;
    std::int64_t mem_used = 0, mem_total = 0;
    bool swap_present = false;
    std::int64_t swap_used = 0, swap_total = 0;
    bool disk_present = false;
    std::int64_t disk_used = 0, disk_total = 0;
    bool os_present = false;
    std::string os_arch, os_name, os_version;
    bool uptime_present = false;
    std::int64_t uptime_ms = 0;
    // Per-interface network rolling averages (SystemStatistics.net, field 8).
    bool net_present = false;
    std::map<std::string, NetworkInterfaceSnapshot> net_averages;
};

// Per-time-window statistics for the viewer's timeline overlay.
struct WindowStats {
    bool ticks_present = false;
    int ticks = 0;
    bool cpu_process_present = false;
    double cpu_process = 0.0;
    bool cpu_system_present = false;
    double cpu_system = 0.0;
    bool tps_present = false;
    double tps = 0.0;
    bool mspt_present = false;
    double mspt_median = 0.0;
    double mspt_max = 0.0;
    bool players_present = false;
    int players = 0;
    bool entities_present = false;
    int entities = 0;
    bool chunks_present = false;
    int chunks = 0;
    std::int64_t start_time_ms = 0;
    std::int64_t end_time_ms = 0;
    int duration_ms = 0;
};

// A point-in-time CPU-time reading; two of them yield a usage fraction.
struct CpuSnapshot {
    bool valid = false;
    unsigned long long process_ticks = 0;  // utime + stime
    double process_ticks_per_second = 0.0;
    int cpu_threads = 1;
    unsigned long long system_busy = 0;
    unsigned long long system_total = 0;
    std::int64_t wall_ms = 0;
};

struct CpuUsage {
    bool process_valid = false;
    bool system_valid = false;
    double process = 0.0;  // 0..1 of total host capacity
    double system = 0.0;   // 0..1
};

CpuSnapshot captureCpuSnapshot();
CpuUsage cpuUsageBetween(const CpuSnapshot &before, const CpuSnapshot &after);

// `disk_path` selects the filesystem to report (for example the server working
// directory). These queries run only for health reports and profile export.
SystemStats gatherSystemStats(const std::string &disk_path);
ProcessStats gatherProcessStats();

}  // namespace spark

#endif  // ENDSTONE_SPARK_SYSTEM_STATS_H
