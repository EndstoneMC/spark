#include "application/spark_application.h"

#include <utility>

namespace spark {

SparkApplication::SparkApplication(std::string bds_executable_sha256,
                                   std::filesystem::path profile_storage_dir,
                                   MainThreadDispatcher &dispatcher,
                                   ProfileMetadataProvider &metadata_provider,
                                   ResultNotifier &notifier)
    : statistics_(),
      dispatcher_(dispatcher),
      metadata_provider_(metadata_provider),
      notifier_(notifier),
      profiler_(statistics_, std::move(bds_executable_sha256),
                std::move(profile_storage_dir),
                dispatcher_, metadata_provider_, notifier_),
      health_(statistics_, metadata_provider_),
      tick_monitor_(notifier_)
{
    registerCommands();
    profiler_.setPingSamplesProvider([this]() { return health_.pingSamples(); });
    profiler_.setNetworkSnapshotProvider([this]() { return health_.networkSnapshots(); });
}

void SparkApplication::registerCommands()
{
    registry_.registerCommand(
        "profiler", "start/stop/info/cancel an execution or allocation profile",
        [this](CommandSender &sender, const Arguments &args) {
            const std::string &action = args.subCommand();
            if (action == "start") {
                profiler_.cmdStart(sender, args);
            }
            else if (action == "stop") {
                profiler_.cmdStop(sender, args);
            }
            else if (action == "cancel") {
                profiler_.cmdCancel(sender);
            }
            else {
                profiler_.cmdInfo(sender);
            }
        });
    registry_.registerCommand(
        "tps", "rolling TPS, MSPT percentiles, and CPU usage",
        [this](CommandSender &sender, const Arguments &) {
            health_.cmdTps(sender);
        });
    registry_.registerCommand(
        "ping", "player ping RTT statistics",
        [this](CommandSender &sender, const Arguments &args) {
            health_.cmdPing(sender, args);
        });
    registry_.registerCommand(
        "health", "performance and host resource report",
        [this](CommandSender &sender, const Arguments &) {
            health_.cmdHealth(sender);
        });
    registry_.registerCommand(
        "tickmonitor", "report unusually long ticks",
        [this](CommandSender &sender, const Arguments &args) {
            tick_monitor_.cmdTickMonitor(sender, args);
        });
}

bool SparkApplication::dispatchCommand(CommandSender &sender,
                                       const std::vector<std::string> &tokens)
{
    return registry_.dispatch(sender, tokens);
}

void SparkApplication::onTick(double mspt)
{
    if (statistics_.onTick(mspt)) {
        statistics_.recordPlayerCount(metadata_provider_.playerCount());
    }
    // Poll ping every ~10 seconds (200 ticks at 20 TPS).
    if (++tick_counter_ % 200 == 0) {
        health_.pollPing();
    }
    // Poll network every ~60 seconds (1200 ticks at 20 TPS).
    if (tick_counter_ % 1200 == 0) {
        health_.pollNetwork();
    }
    tick_monitor_.onTick(mspt);
    profiler_.onTick(mspt);
}

void SparkApplication::shutdown()
{
    profiler_.shutdown();
}

}  // namespace spark
