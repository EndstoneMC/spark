#include <atomic>
#include <cstdint>
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
#include "platform/endstone/health_command.h"
#include "platform/endstone/profiler_controller.h"
#include "platform/endstone/tick_monitor_controller.h"
#include "spark_constants.h"
#include "stats/executable_hash.h"
#include "stats/statistics_service.h"

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
        health_ = std::make_unique<spark::endstone_adapter::HealthCommands>(*this, statistics_);
        tick_monitor_ = std::make_unique<spark::endstone_adapter::TickMonitorController>(*this);
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
        getServer().getScheduler().cancelTasks(*this);
        tick_task_.reset();

        std::string shutdown_error;
        if (controller_ && !controller_->shutdownProfiler(shutdown_error)) {
            std::fprintf(stderr, "[spark] profiler shutdown failed before plugin unload: %s\n",
                         shutdown_error.c_str());
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
            health_->cmdTps(sender);
        }
        else if (module == "health") {
            health_->cmdHealth(sender);
        }
        else if (module == "tickmonitor") {
            std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
            tick_monitor_->cmdTickMonitor(sender, spark::Arguments(rest));
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
        if (tick_monitor_) {
            tick_monitor_->onTick(mspt);
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

    spark::StatisticsService statistics_;
    std::string bds_executable_sha256_;
    std::atomic<std::uint64_t> main_tid_{0};
    std::shared_ptr<endstone::Task> tick_task_;
    std::unique_ptr<spark::endstone_adapter::ProfilerController> controller_;
    std::unique_ptr<spark::endstone_adapter::HealthCommands> health_;
    std::unique_ptr<spark::endstone_adapter::TickMonitorController> tick_monitor_;
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
