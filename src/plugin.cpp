#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <endstone/endstone.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "command/arguments.h"
#include "core/util/format.h"
#include "platform/endstone/profiler_controller.h"
#include "spark_constants.h"
#include "stats/executable_hash.h"
#include "stats/statistics_service.h"
#include "stats/system_stats.h"
#include "stats/tick_monitor.h"

namespace {

using endstone::ColorFormat;

std::uint64_t currentThreadId()
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
}

}  // namespace

class SparkPlugin : public endstone::Plugin {
public:
    void onEnable() override
    {
        statistics_.start();
        statistics_.recordPlayerCount(
            static_cast<long>(getServer().getOnlinePlayers().size()));
        std::string hash_error;
        bds_executable_sha256_ = spark::currentExecutableSha256(hash_error);
        if (bds_executable_sha256_.empty()) {
            getLogger().warning("Unable to identify the BDS executable: {}", hash_error);
        }
        controller_ = std::make_unique<spark::endstone_adapter::ProfilerController>(
            *this, statistics_, bds_executable_sha256_);
        tick_task_ = getServer().getScheduler().runTaskTimer(
            *this, [this]() { onServerTick(); }, 0, 1);
        getLogger().info("endstone-spark v{} enabled. Run {}/spark{} to get started.", spark::kVersion,
                         ColorFormat::Gold, ColorFormat::Reset);
    }

    void onDisable() override
    {
        if (controller_) {
            controller_->shutdown();
        }
        // The export thread may have queued announceResult() just before it
        // exited. Remove every scheduler-owned callback before proving backend
        // quiescence so no task can re-enter old plugin code during/after reload.
        getServer().getScheduler().cancelTasks(*this);
        tick_task_.reset();

        std::string shutdown_error;
        if (controller_ && !controller_->shutdownProfiler(shutdown_error)) {
            std::fprintf(stderr, "[spark] profiler shutdown failed before plugin unload: %s\n",
                         shutdown_error.c_str());
            // Endstone cannot veto unload from onDisable(). Continuing would let
            // allocator entries or in-flight thunks reference an unloaded DLL.
            // Fail closed instead of pinning/leaking old plugin code across reload.
            std::abort();
        }
    }

    bool onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                   const std::vector<std::string> &args) override
    {
        if (command.getName() != "spark") {
            return false;
        }
        if (main_tid_.load() == 0 && getServer().isPrimaryThread()) {
            main_tid_.store(currentThreadId());
        }

        std::vector<std::string> tokens;
        for (const auto &arg : args) {
            auto parsed = spark::Arguments::tokenize(arg);
            tokens.insert(tokens.end(), parsed.begin(), parsed.end());
        }
        std::string module = tokens.empty() ? std::string() : tokens[0];

        if (module.empty()) {
            sendHelp(sender);
        }
        else if (module == "tps") {
            cmdTps(sender);
        }
        else if (module == "health") {
            cmdHealth(sender);
        }
        else if (module == "tickmonitor") {
            std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
            cmdTickMonitor(sender, spark::Arguments(rest));
        }
        else if (module == "profiler") {
            std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
            controller_->cmdProfiler(sender, spark::Arguments(rest), main_tid_.load());
        }
        else {
            sendHelp(sender);
        }
        return true;
    }

    void onServerTick()
    {
        if (main_tid_.load() == 0) {
            main_tid_.store(currentThreadId());
        }
        const double mspt = getServer().getCurrentMillisecondsPerTick();
        if (statistics_.onTick(mspt)) {
            statistics_.recordPlayerCount(
                static_cast<long>(getServer().getOnlinePlayers().size()));
        }
        if (tick_monitor_.running()) {
            processTickMonitor(mspt);
        }
        if (controller_) {
            controller_->onTick(mspt);
        }
    }

private:
    void sendHelp(endstone::CommandSender &sender)
    {
        sender.sendMessage("{}endstone-spark {}v{}", ColorFormat::Gold, ColorFormat::Gray, spark::kVersion);
        sender.sendMessage("{}/spark profiler start [flags] {}- start an execution or allocation profile",
                           ColorFormat::Yellow, ColorFormat::Gray);
        sender.sendMessage("{}/spark profiler stop {}- stop profiling and finalize the profile", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage("{}/spark profiler info {}- show status of the running profiler", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage("{}/spark profiler cancel {}- stop profiling without generating a profile", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage(
            "{}/spark tps {}- rolling TPS, MSPT percentiles, and CPU usage",
            ColorFormat::Yellow, ColorFormat::Gray);
        sender.sendMessage(
            "{}/spark health {}- performance and host resource report",
            ColorFormat::Yellow, ColorFormat::Gray);
        sender.sendMessage("{}/spark tickmonitor {}- report unusually long ticks", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage("{}Modes: --alloc, --alloc-live-only", ColorFormat::Gray);
        sender.sendMessage("{}Thread selection: --thread <name|*>, --regex", ColorFormat::Gray);
        sender.sendMessage("{}Execution only: --include-sleeping", ColorFormat::Gray);
        sender.sendMessage(
            "{}Flags: --interval <ms|bytes>, --timeout <seconds>, --only-ticks-over <ms>",
            ColorFormat::Gray);
        sender.sendMessage("{}       --save-to-file (plugins/spark/profiles), --comment <text>",
                           ColorFormat::Gray);
    }

    void cmdTps(endstone::CommandSender &sender)
    {
        sendPerformanceReport(sender, statistics_.snapshot());
    }

    void sendPerformanceReport(endstone::CommandSender &sender,
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

    void cmdHealth(endstone::CommandSender &sender)
    {
        const spark::StatisticsSnapshot statistics = statistics_.snapshot();
        sendPerformanceReport(sender, statistics);

        const spark::ProcessStats process = spark::gatherProcessStats();
        const spark::SystemStats system = spark::gatherSystemStats(".");
        const std::int64_t uptime = std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now() -
                                        getServer().getStartTime())
                                        .count();
        sender.sendMessage("{}Uptime: {}{}", ColorFormat::Gold,
                           ColorFormat::Gray, spark::formatDuration(uptime));
        sender.sendMessage("{}Players online: {}{}", ColorFormat::Gold,
                           ColorFormat::Gray,
                           getServer().getOnlinePlayers().size());

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

    void cmdTickMonitor(endstone::CommandSender &sender, const spark::Arguments &args)
    {
        if (tick_monitor_.running()) {
            tick_monitor_.stop();
            sender.sendMessage("{}Tick monitor disabled.", ColorFormat::Gold);
            return;
        }

        const bool has_percentage = args.boolFlag("threshold");
        const bool has_duration = args.boolFlag("threshold-tick");
        if (has_percentage && has_duration) {
            sender.sendErrorMessage("Choose either --threshold or --threshold-tick, not both.");
            return;
        }

        spark::TickMonitorConfig config;
        if (has_percentage) {
            auto threshold = args.doubleFlag("threshold");
            if (!threshold || *threshold <= 0.0) {
                sender.sendErrorMessage("The percentage threshold must be a positive number.");
                return;
            }
            config.mode = spark::TickMonitorMode::Percentage;
            config.threshold = *threshold;
        }
        else if (has_duration) {
            auto threshold = args.doubleFlag("threshold-tick");
            if (!threshold || *threshold <= 0.0) {
                sender.sendErrorMessage("The tick duration threshold must be a positive number of milliseconds.");
                return;
            }
            config.mode = spark::TickMonitorMode::Duration;
            config.threshold = *threshold;
        }

        if (!tick_monitor_.start(config)) {
            sender.sendErrorMessage("Unable to start the tick monitor with the requested threshold.");
            return;
        }
        tick_monitor_sender_ = sender.getName();
        sender.sendMessage("{}Tick monitor started.{} Calculating the baseline over 120 ticks (about 6 seconds).",
                           ColorFormat::Gold, ColorFormat::Gray);
    }

    void processTickMonitor(double mspt)
    {
        spark::TickMonitorUpdate update = tick_monitor_.onTick(mspt);
        char message[256];
        if (update.setup_completed) {
            std::snprintf(message, sizeof(message),
                          "Tick monitor baseline ready: min %.2fms, average %.2fms, max %.2fms.",
                          update.setup_min_ms, update.baseline_ms, update.setup_max_ms);
            announce(tick_monitor_sender_, message);

            if (tick_monitor_.config().mode == spark::TickMonitorMode::Duration) {
                std::snprintf(message, sizeof(message), "Reporting ticks longer than %.2fms.",
                              tick_monitor_.config().threshold);
            }
            else {
                std::snprintf(message, sizeof(message),
                              "Reporting ticks more than %.2f%% above the baseline.",
                              tick_monitor_.config().threshold);
            }
            announce(tick_monitor_sender_, message);
        }
        if (update.report) {
            std::snprintf(message, sizeof(message),
                          "Tick #%llu lasted %.2fms (%.2f%% change from the %.2fms baseline).",
                          static_cast<unsigned long long>(update.tick), update.duration_ms,
                          update.percentage_change, update.baseline_ms);
            announce(tick_monitor_sender_, message);
        }
    }

    void announce(const std::string &sender_name, const std::string &text)
    {
        getLogger().info("{}", text);
        auto player = getServer().getPlayer(sender_name);
        if (player) {
            player->sendMessage("{}[spark] {}{}", ColorFormat::Gold, ColorFormat::Reset, text);
        }
    }

    spark::StatisticsService statistics_;
    spark::TickMonitor tick_monitor_;
    std::string tick_monitor_sender_ = "CONSOLE";
    std::string bds_executable_sha256_;
    std::atomic<std::uint64_t> main_tid_{0};
    std::shared_ptr<endstone::Task> tick_task_;
    std::unique_ptr<spark::endstone_adapter::ProfilerController> controller_;
};

ENDSTONE_PLUGIN("spark", "0.4.1", SparkPlugin)
{
    description = "spark profiler for Endstone - find what's slowing your server down.";
    authors = {"ReallocAll <ReallocAll@outlook.com>"};
    prefix = "Spark";
    load = endstone::PluginLoadOrder::PostWorld;

    command("spark")
        .description("spark profiler")
        .usages("/spark", "/spark (tps|health|tickmonitor)<module: SparkStatusModule>",
                "/spark (profiler)<module: SparkProfilerModule> "
                "(start|stop|info|cancel)[action: SparkProfilerAction] [flags: message]")
        .permissions("endstone.command.spark");

    permission("endstone.command.spark")
        .description("Allows use of the spark profiler")
        .default_(endstone::PermissionDefault::Operator);
}
