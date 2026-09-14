#include "application/health/health_report.h"
#include "application/profiler/platform_metadata_capture.h"

#include <algorithm>
#include <utility>

#include "core/profiler/profiler.h"
#include "core/stats/ping_statistics.h"
#include "core/stats/system_stats.h"
#include "core/util/format.h"
#include "proto/sampler_data.h"
#include "spark_constants.h"

namespace spark {

void sendPerformanceReport(CommandSender &sender, const StatisticsSnapshot &stats)
{
    sender.sendMessage("{}TPS {}(5s/10s/1m/5m/15m){}: {} / {} / {} / {} / {}", kColorGold, kColorGray, kColorReset,
                       formatTpsValue(stats.tps.last_5s), formatTpsValue(stats.tps.last_10s),
                       formatTpsValue(stats.tps.last_1m), formatTpsValue(stats.tps.last_5m),
                       formatTpsValue(stats.tps.last_15m));
    sender.sendMessage("{}MSPT 10s {}(mean/min/median/p95/max){}: {}", kColorGold, kColorGray, kColorReset,
                       formatMsptDistribution(stats.mspt.last_10s));
    sender.sendMessage("{}MSPT 1m  {}(mean/min/median/p95/max){}: {}", kColorGold, kColorGray, kColorReset,
                       formatMsptDistribution(stats.mspt.last_1m));
    sender.sendMessage("{}MSPT 5m  {}(mean/min/median/p95/max){}: {}", kColorGold, kColorGray, kColorReset,
                       formatMsptDistribution(stats.mspt.last_5m));
    sender.sendMessage("{}Process CPU {}(10s/1m/15m){}: {} / {} / {}", kColorGold, kColorGray, kColorReset,
                       formatCpuValue(stats.cpu.process_last_10s), formatCpuValue(stats.cpu.process_last_1m),
                       formatCpuValue(stats.cpu.process_last_15m));
    sender.sendMessage("{}System CPU {}(10s/1m/15m){}: {} / {} / {}", kColorGold, kColorGray, kColorReset,
                       formatCpuValue(stats.cpu.system_last_10s), formatCpuValue(stats.cpu.system_last_1m),
                       formatCpuValue(stats.cpu.system_last_15m));

    const std::int64_t history_seconds = (stats.history_span_ms + 999) / 1000;
    if (stats.history_span_ms < StatisticsService::kMaximumHistoryMs) {
        sender.sendMessage("{}Statistics history: {}{} {}(longer windows currently use the available history)",
                           kColorGold, kColorGray, formatDuration(history_seconds), kColorGray);
    }
}

void showHealthReport(CommandSender &sender, StatisticsService &statistics, ProfileMetadataProvider &metadata_provider,
                      const std::map<std::string, NetworkInterfaceSnapshot> &network_snapshots, bool detailed_memory,
                      bool detailed_network)
{
    const StatisticsSnapshot snapshot = statistics.snapshot();
    sendPerformanceReport(sender, snapshot);

    const ProcessStats process = gatherProcessStats();
    const SystemStats system = gatherSystemStats(".");
    sender.sendMessage("{}Uptime: {}{}", kColorGold, kColorGray,
                       formatDuration(metadata_provider.serverUptimeSeconds()));
    sender.sendMessage("{}Players online: {}{}", kColorGold, kColorGray, metadata_provider.playerCount());

    if (process.rss_present) {
        sender.sendMessage("{}Process RSS: {}{}", kColorGold, kColorGray,
                           formatBytes(static_cast<std::uint64_t>(process.rss_bytes)));
    }
    if (detailed_memory && process.virtual_present) {
        sender.sendMessage("{}Process virtual memory: {}{}", kColorGold, kColorGray,
                           formatBytes(static_cast<std::uint64_t>(process.virtual_bytes)));
    }
    if (detailed_memory && process.threads_present) {
        sender.sendMessage("{}Process threads: {}{}", kColorGold, kColorGray, process.threads);
    }
    if (system.memory_present) {
        sender.sendMessage("{}System memory {}(used/total){}: {} / {}", kColorGold, kColorGray, kColorReset,
                           formatBytes(static_cast<std::uint64_t>(system.mem_used)),
                           formatBytes(static_cast<std::uint64_t>(system.mem_total)));
    }
    if (detailed_memory && system.swap_present) {
        sender.sendMessage("{}Swap/page file {}(used/total){}: {} / {}", kColorGold, kColorGray, kColorReset,
                           formatBytes(static_cast<std::uint64_t>(system.swap_used)),
                           formatBytes(static_cast<std::uint64_t>(system.swap_total)));
    }
    if (system.disk_present) {
        sender.sendMessage("{}Disk {}(used/total){}: {} / {}", kColorGold, kColorGray, kColorReset,
                           formatBytes(static_cast<std::uint64_t>(system.disk_used)),
                           formatBytes(static_cast<std::uint64_t>(system.disk_total)));
    }
    if (system.cpu_present) {
        sender.sendMessage("{}CPU: {}{} {}({} logical processors)", kColorGold, kColorGray,
                           system.cpu_model.empty() ? "unknown model" : system.cpu_model, kColorGray,
                           system.cpu_threads);
    }
    if (system.os_present) {
        sender.sendMessage("{}OS: {}{} {} {}", kColorGold, kColorGray, system.os_name, system.os_version,
                           system.os_arch);
    }

    std::vector<std::string> network_lines;
    for (const auto &[name, snapshot] : network_snapshots) {
        const auto append_direction = [&](const char *direction, const NetworkRateValues &bytes,
                                          const NetworkRateValues &packets) {
            if (!bytes.present || !packets.present || (!detailed_network && bytes.mean <= 0.0 && packets.mean <= 0.0)) {
                return;
            }
            const auto bytes_per_second = static_cast<std::uint64_t>((std::max)(0.0, bytes.mean));
            const auto packets_per_second = static_cast<std::uint64_t>((std::max)(0.0, packets.mean));
            std::string message = "  " + kColorGreen + formatBytes(bytes_per_second) + "/s";
            message += kColorGray;
            message += " / ";
            message += kColorReset;
            message += std::to_string(packets_per_second);
            message += " pps";
            message += kColorGray;
            message += " (";
            message += name;
            message += " ";
            message += direction;
            message += ")";
            network_lines.push_back(std::move(message));
        };
        append_direction("RX", snapshot.rx_bytes_per_second, snapshot.rx_packets_per_second);
        append_direction("TX", snapshot.tx_bytes_per_second, snapshot.tx_packets_per_second);
    }
    if (!network_lines.empty()) {
        sender.sendMessage("{}Network usage {}(system, last 15m mean){}:", kColorGold, kColorGray, kColorReset);
        for (const std::string &line : network_lines) {
            sender.sendMessage(line);
        }
    }
}

HealthData captureHealthData(StatisticsService &statistics, ProfileMetadataProvider &metadata_provider,
                             const std::string &sender_name, bool sender_is_player, std::int64_t now_ms,
                             const std::vector<int> &ping_samples,
                             const std::map<std::string, NetworkInterfaceSnapshot> &network_snapshots,
                             std::string sender_unique_id)
{
    ExportContext context;
    gatherPlatformServerMetadata(metadata_provider, context, now_ms);
    metadata_provider.gatherWorldMetadata(context.world, context.minecraft_version);
    context.statistics = statistics.snapshot();
    context.metrics = statistics.metricsSnapshot();
    context.system_stats = gatherSystemStats(".");
    context.system_stats.uptime_present = true;
    context.system_stats.uptime_ms = context.uptime_ms;
    context.system_stats.present = true;
    context.window_stats = statistics.profileWindows(0, now_ms);
    context.ping_samples = ping_samples;
    context.net_snapshots = network_snapshots;

    HealthData data;
    data.creator_name = sender_name;
    data.creator_is_player = sender_is_player;
    data.creator_unique_id = sender_is_player ? std::move(sender_unique_id) : std::string{};
    data.endstone_version = context.endstone_version;
    data.minecraft_version = context.minecraft_version;
    data.generated_time_ms = now_ms;
    data.world = std::move(context.world);

    data.platform_stats.present = true;
    data.platform_stats.player_count = context.player_count;
    data.platform_stats.online_mode = context.online_mode;
    data.platform_stats.uptime_ms = context.uptime_ms;
    const ProcessStats process = gatherProcessStats();
    data.platform_stats.process_mem_present = process.rss_present;
    data.platform_stats.process_mem_bytes = process.rss_bytes;
    data.platform_stats.process_virtual_present = process.virtual_present;
    data.platform_stats.process_virtual_bytes = process.virtual_bytes;
    if (!context.ping_samples.empty()) {
        PingRollingAverage average(PingStatistics::kWindowSize);
        for (const int value : context.ping_samples) {
            average.add(value);
        }
        data.platform_stats.ping_present = true;
        data.platform_stats.ping_mean = average.mean();
        data.platform_stats.ping_max = static_cast<double>(average.max());
        data.platform_stats.ping_min = static_cast<double>(average.min());
        data.platform_stats.ping_median = static_cast<double>(average.median());
        data.platform_stats.ping_p95 = static_cast<double>(average.percentile95th());
    }

    data.system_stats = context.system_stats;
    if (!context.net_snapshots.empty()) {
        data.system_stats.net_present = true;
        data.system_stats.net_averages = context.net_snapshots;
    }
    data.statistics = context.statistics;
    data.metrics = context.metrics;
    data.plugins = context.plugins;
    data.server_configurations = context.server_configurations;
    data.window_stats = context.window_stats;
    if (!context.bds_executable_sha256.empty()) {
        data.extra_platform_metadata["BDS executable SHA-256"] = "\"" + context.bds_executable_sha256 + "\"";
    }
    data.extra_platform_metadata["Statistics history available ms"] =
        std::to_string(context.statistics.history_span_ms);
    return data;
}

}  // namespace spark
