#include "platform/endstone/health_command.h"

#include <chrono>
#include <cstdint>

#include <endstone/endstone.hpp>

#include "core/util/format.h"
#include "stats/statistics_service.h"
#include "stats/system_stats.h"

namespace spark::endstone_adapter {

using endstone::ColorFormat;

HealthCommands::HealthCommands(::endstone::Plugin &plugin,
                               spark::StatisticsService &statistics)
    : plugin_(plugin), statistics_(statistics)
{
}

void HealthCommands::cmdTps(::endstone::CommandSender &sender)
{
    sendPerformanceReport(sender, statistics_.snapshot());
}

void HealthCommands::sendPerformanceReport(::endstone::CommandSender &sender,
                                           const spark::StatisticsSnapshot &stats)
{
    sender.sendMessage(
        "{}TPS {}(5s/10s/1m/5m/15m){}: {} / {} / {} / {} / {}",
        ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
        spark::formatTpsValue(stats.tps.last_5s),
        spark::formatTpsValue(stats.tps.last_10s),
        spark::formatTpsValue(stats.tps.last_1m),
        spark::formatTpsValue(stats.tps.last_5m),
        spark::formatTpsValue(stats.tps.last_15m));
    sender.sendMessage(
        "{}MSPT 10s {}(mean/min/median/p95/max){}: {}",
        ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
        spark::formatMsptDistribution(stats.mspt.last_10s));
    sender.sendMessage(
        "{}MSPT 1m  {}(mean/min/median/p95/max){}: {}",
        ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
        spark::formatMsptDistribution(stats.mspt.last_1m));
    sender.sendMessage(
        "{}MSPT 5m  {}(mean/min/median/p95/max){}: {}",
        ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
        spark::formatMsptDistribution(stats.mspt.last_5m));
    sender.sendMessage(
        "{}Process CPU {}(10s/1m/15m){}: {} / {} / {}",
        ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
        spark::formatCpuValue(stats.cpu.process_last_10s),
        spark::formatCpuValue(stats.cpu.process_last_1m),
        spark::formatCpuValue(stats.cpu.process_last_15m));
    sender.sendMessage(
        "{}System CPU {}(10s/1m/15m){}: {} / {} / {}",
        ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
        spark::formatCpuValue(stats.cpu.system_last_10s),
        spark::formatCpuValue(stats.cpu.system_last_1m),
        spark::formatCpuValue(stats.cpu.system_last_15m));

    const std::int64_t history_seconds =
        (stats.history_span_ms + 999) / 1000;
    if (stats.history_span_ms < spark::StatisticsService::kMaximumHistoryMs) {
        sender.sendMessage(
            "{}Statistics history: {}{} {}(longer windows currently use the available history)",
            ColorFormat::Gold, ColorFormat::Gray,
            spark::formatDuration(history_seconds), ColorFormat::Gray);
    }
}

void HealthCommands::cmdHealth(::endstone::CommandSender &sender)
{
    const spark::StatisticsSnapshot statistics = statistics_.snapshot();
    sendPerformanceReport(sender, statistics);

    const spark::ProcessStats process = spark::gatherProcessStats();
    const spark::SystemStats system = spark::gatherSystemStats(".");
    const std::int64_t uptime = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now() -
                                    plugin_.getServer().getStartTime())
                                    .count();
    sender.sendMessage("{}Uptime: {}{}", ColorFormat::Gold,
                       ColorFormat::Gray, spark::formatDuration(uptime));
    sender.sendMessage("{}Players online: {}{}", ColorFormat::Gold,
                       ColorFormat::Gray,
                       plugin_.getServer().getOnlinePlayers().size());

    if (process.rss_present && process.virtual_present) {
        sender.sendMessage(
            "{}Process memory {}(RSS/virtual){}: {} / {}",
            ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
            spark::formatBytes(static_cast<std::uint64_t>(process.rss_bytes)),
            spark::formatBytes(
                static_cast<std::uint64_t>(process.virtual_bytes)));
    }
    else if (process.rss_present) {
        sender.sendMessage("{}Process RSS: {}{}", ColorFormat::Gold,
                           ColorFormat::Gray,
                           spark::formatBytes(static_cast<std::uint64_t>(
                               process.rss_bytes)));
    }
    else if (process.virtual_present) {
        sender.sendMessage("{}Process virtual memory: {}{}",
                           ColorFormat::Gold, ColorFormat::Gray,
                           spark::formatBytes(static_cast<std::uint64_t>(
                               process.virtual_bytes)));
    }
    if (process.threads_present) {
        sender.sendMessage("{}Process threads: {}{}", ColorFormat::Gold,
                           ColorFormat::Gray, process.threads);
    }
    if (system.memory_present) {
        sender.sendMessage("{}System memory {}(used/total){}: {} / {}",
                           ColorFormat::Gold, ColorFormat::Gray,
                           ColorFormat::Reset,
                           spark::formatBytes(static_cast<std::uint64_t>(
                               system.mem_used)),
                           spark::formatBytes(static_cast<std::uint64_t>(
                               system.mem_total)));
    }
    if (system.swap_present) {
        sender.sendMessage(
            "{}Swap/page file {}(used/total){}: {} / {}",
            ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
            spark::formatBytes(
                static_cast<std::uint64_t>(system.swap_used)),
            spark::formatBytes(
                static_cast<std::uint64_t>(system.swap_total)));
    }
    if (system.disk_present) {
        sender.sendMessage("{}Disk {}(used/total){}: {} / {}",
                           ColorFormat::Gold, ColorFormat::Gray,
                           ColorFormat::Reset,
                           spark::formatBytes(static_cast<std::uint64_t>(
                               system.disk_used)),
                           spark::formatBytes(static_cast<std::uint64_t>(
                               system.disk_total)));
    }
    if (system.cpu_present) {
        sender.sendMessage("{}CPU: {}{} {}({} logical processors)",
                           ColorFormat::Gold, ColorFormat::Gray,
                           system.cpu_model.empty() ? "unknown model"
                                                    : system.cpu_model,
                           ColorFormat::Gray, system.cpu_threads);
    }
    if (system.os_present) {
        sender.sendMessage("{}OS: {}{} {} {}", ColorFormat::Gold,
                           ColorFormat::Gray, system.os_name,
                           system.os_version, system.os_arch);
    }
}

}  // namespace spark::endstone_adapter
