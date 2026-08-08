#include "application/health/health_command.h"

#include <cstdint>
#include <utility>

#include "core/command/arguments.h"
#include "core/stats/system_stats.h"
#include "core/util/format.h"

namespace spark {

HealthCommand::HealthCommand(StatisticsService &statistics,
                             ProfileMetadataProvider &metadata_provider)
    : statistics_(statistics), metadata_provider_(metadata_provider)
{
    // Lazily create PingStatistics if the platform provides a PlayerPingProvider.
    if (auto *ping_provider = metadata_provider_.playerPingProvider()) {
        ping_statistics_ = std::make_unique<PingStatistics>(*ping_provider);
    }
}

void HealthCommand::pollPing()
{
    if (ping_statistics_) {
        ping_statistics_->poll();
    }
}

std::vector<int> HealthCommand::pingSamples() const
{
    if (!ping_statistics_) {
        return {};
    }
    return ping_statistics_->rollingAverage().rawSamples();
}

void HealthCommand::pollNetwork()
{
    network_monitor_.poll();
}

std::map<std::string, NetworkInterfaceSnapshot> HealthCommand::networkSnapshots() const
{
    return network_monitor_.snapshot();
}

void HealthCommand::cmdTps(CommandSender &sender)
{
    sendPerformanceReport(sender, statistics_.snapshot());
}

void HealthCommand::cmdPing(CommandSender &sender, const Arguments &args)
{
    if (!ping_statistics_) {
        sender.sendMessage("{}Ping data is not available on this platform.{}", kColorGold, kColorGray);
        return;
    }

    // Query specific player
    auto players = args.stringFlag("player");
    if (!players.empty()) {
        for (const std::string &player_name : players) {
            PlayerPing ping = ping_statistics_->query(player_name);
            if (!ping.found()) {
                sender.sendMessage("{}Ping data is not available for '{}'.{}", kColorGold, kColorGray, kColorReset);
                sender.sendMessage("  {}", player_name);
            } else {
                sender.sendMessage("{}Player {}{} {}has {}{} ms ping.{}",
                                   kColorGold, kColorReset, ping.name,
                                   kColorGray, kColorGreen, ping.ping, kColorReset);
            }
        }
        return;
    }

    PingSummary summary = ping_statistics_->currentSummary();
    const PingRollingAverage &average = ping_statistics_->rollingAverage();

    if (summary.total() == 0 && average.samples() == 0) {
        sender.sendMessage("{}There is not enough data to show ping averages yet. Please try again later.{}",
                           kColorGold, kColorGray);
        return;
    }

    sender.sendMessage("{}Average Pings {}(min/med/95%ile/max ms){} from now, last 15m:",
                       kColorGold, kColorGray, kColorReset);
    sender.sendMessage("  {} ;  {}",
                       formatPingRtts(summary),
                       formatPingRtts(average));
}

void HealthCommand::sendPerformanceReport(CommandSender &sender,
                                          const StatisticsSnapshot &stats)
{
    sender.sendMessage(
        "{}TPS {}(5s/10s/1m/5m/15m){}: {} / {} / {} / {} / {}",
        kColorGold, kColorGray, kColorReset,
        formatTpsValue(stats.tps.last_5s),
        formatTpsValue(stats.tps.last_10s),
        formatTpsValue(stats.tps.last_1m),
        formatTpsValue(stats.tps.last_5m),
        formatTpsValue(stats.tps.last_15m));
    sender.sendMessage(
        "{}MSPT 10s {}(mean/min/median/p95/max){}: {}",
        kColorGold, kColorGray, kColorReset,
        formatMsptDistribution(stats.mspt.last_10s));
    sender.sendMessage(
        "{}MSPT 1m  {}(mean/min/median/p95/max){}: {}",
        kColorGold, kColorGray, kColorReset,
        formatMsptDistribution(stats.mspt.last_1m));
    sender.sendMessage(
        "{}MSPT 5m  {}(mean/min/median/p95/max){}: {}",
        kColorGold, kColorGray, kColorReset,
        formatMsptDistribution(stats.mspt.last_5m));
    sender.sendMessage(
        "{}Process CPU {}(10s/1m/15m){}: {} / {} / {}",
        kColorGold, kColorGray, kColorReset,
        formatCpuValue(stats.cpu.process_last_10s),
        formatCpuValue(stats.cpu.process_last_1m),
        formatCpuValue(stats.cpu.process_last_15m));
    sender.sendMessage(
        "{}System CPU {}(10s/1m/15m){}: {} / {} / {}",
        kColorGold, kColorGray, kColorReset,
        formatCpuValue(stats.cpu.system_last_10s),
        formatCpuValue(stats.cpu.system_last_1m),
        formatCpuValue(stats.cpu.system_last_15m));

    const std::int64_t history_seconds =
        (stats.history_span_ms + 999) / 1000;
    if (stats.history_span_ms < StatisticsService::kMaximumHistoryMs) {
        sender.sendMessage(
            "{}Statistics history: {}{} {}(longer windows currently use the available history)",
            kColorGold, kColorGray,
            formatDuration(history_seconds), kColorGray);
    }
}

void HealthCommand::cmdHealth(CommandSender &sender)
{
    const StatisticsSnapshot statistics = statistics_.snapshot();
    sendPerformanceReport(sender, statistics);

    const ProcessStats process = gatherProcessStats();
    const SystemStats system = gatherSystemStats(".");
    const std::int64_t uptime = metadata_provider_.serverUptimeSeconds();
    sender.sendMessage("{}Uptime: {}{}", kColorGold,
                       kColorGray, formatDuration(uptime));
    sender.sendMessage("{}Players online: {}{}", kColorGold,
                       kColorGray, metadata_provider_.playerCount());

    if (process.rss_present && process.virtual_present) {
        sender.sendMessage(
            "{}Process memory {}(RSS/virtual){}: {} / {}",
            kColorGold, kColorGray, kColorReset,
            formatBytes(static_cast<std::uint64_t>(process.rss_bytes)),
            formatBytes(static_cast<std::uint64_t>(process.virtual_bytes)));
    }
    else if (process.rss_present) {
        sender.sendMessage("{}Process RSS: {}{}", kColorGold,
                           kColorGray,
                           formatBytes(static_cast<std::uint64_t>(process.rss_bytes)));
    }
    else if (process.virtual_present) {
        sender.sendMessage("{}Process virtual memory: {}{}",
                           kColorGold, kColorGray,
                           formatBytes(static_cast<std::uint64_t>(process.virtual_bytes)));
    }
    if (process.threads_present) {
        sender.sendMessage("{}Process threads: {}{}", kColorGold,
                           kColorGray, process.threads);
    }
    if (system.memory_present) {
        sender.sendMessage("{}System memory {}(used/total){}: {} / {}",
                           kColorGold, kColorGray, kColorReset,
                           formatBytes(static_cast<std::uint64_t>(system.mem_used)),
                           formatBytes(static_cast<std::uint64_t>(system.mem_total)));
    }
    if (system.swap_present) {
        sender.sendMessage("{}Swap/page file {}(used/total){}: {} / {}",
                           kColorGold, kColorGray, kColorReset,
                           formatBytes(static_cast<std::uint64_t>(system.swap_used)),
                           formatBytes(static_cast<std::uint64_t>(system.swap_total)));
    }
    if (system.disk_present) {
        sender.sendMessage("{}Disk {}(used/total){}: {} / {}",
                           kColorGold, kColorGray, kColorReset,
                           formatBytes(static_cast<std::uint64_t>(system.disk_used)),
                           formatBytes(static_cast<std::uint64_t>(system.disk_total)));
    }
    if (system.cpu_present) {
        sender.sendMessage("{}CPU: {}{} {}({} logical processors)",
                           kColorGold, kColorGray,
                           system.cpu_model.empty() ? "unknown model" : system.cpu_model,
                           kColorGray, system.cpu_threads);
    }
    if (system.os_present) {
        sender.sendMessage("{}OS: {}{} {} {}", kColorGold,
                           kColorGray, system.os_name,
                           system.os_version, system.os_arch);
    }

    auto net_snapshots = network_monitor_.snapshot();
    if (!net_snapshots.empty()) {
        sender.sendMessage("{}Network {}(RX/TX bytes/s, last 15m mean){}:", kColorGold, kColorGray, kColorReset);
        for (const auto &[name, snap] : net_snapshots) {
            if (!snap.rx_bytes_per_second.present || !snap.tx_bytes_per_second.present) {
                continue;
            }
            sender.sendMessage("  {}{}: {} {}{}/s{}  {}{}/s{}",
                               kColorGray, name,
                               kColorGreen, formatBytes(static_cast<std::uint64_t>(snap.rx_bytes_per_second.mean)),
                               kColorGray, kColorGreen,
                               formatBytes(static_cast<std::uint64_t>(snap.tx_bytes_per_second.mean)),
                               kColorGray);
        }
    }
}

}  // namespace spark
