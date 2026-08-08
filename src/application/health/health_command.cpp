#include "application/health/health_command.h"

#include <cstdint>

#include "core/stats/system_stats.h"
#include "core/util/format.h"

namespace spark {

HealthCommand::HealthCommand(StatisticsService &statistics,
                             ProfileMetadataProvider &metadata_provider)
    : statistics_(statistics), metadata_provider_(metadata_provider)
{
}

void HealthCommand::cmdTps(CommandSender &sender)
{
    sendPerformanceReport(sender, statistics_.snapshot());
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
}

}  // namespace spark
