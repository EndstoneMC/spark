#include "application/spark_application.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <utility>

#include "native/sampler/heartbeat.h"
#include "net/gzip.h"
#include "net/profile_file.h"

namespace spark {

SparkApplication::SparkApplication(std::string bds_executable_sha256,
                                   std::filesystem::path profile_storage_dir,
                                   std::filesystem::path activity_log_file,
                                   SparkConfig config,
                                   MainThreadDispatcher &dispatcher,
                                   ProfileMetadataProvider &metadata_provider,
                                   ResultNotifier &notifier)
    : statistics_(),
      config_(std::move(config)),
      dispatcher_(dispatcher),
      metadata_provider_(metadata_provider),
      notifier_(notifier),
      profiler_(statistics_, std::move(bds_executable_sha256),
                profile_storage_dir,
                config_.bytebin_url, config_.viewer_url,
                config_.background_profiler_enabled,
                config_.background_profiler_interval,
                config_.background_profiler_thread_grouper,
                config_.background_profiler_thread_dumper,
                config_,
                dispatcher_, metadata_provider_, notifier_),
      health_(statistics_, metadata_provider_,
              config_.bytebin_url, config_.viewer_url),
      activity_log_(std::move(activity_log_file)),
      activity_command_(activity_log_),
      tick_monitor_(notifier_),
      watchdog_(server_heartbeat_)
{
    recovery_dir_ = profile_storage_dir / "recovery";
    activity_log_.load();
    registerCommands();
    profiler_.setPingSamplesProvider([this]() { return health_.pingSamples(); });
    profiler_.setNetworkSnapshotProvider([this]() { return health_.networkSnapshots(); });
    profiler_.setActivityLogProvider([this]() -> ActivityLog * { return &activity_log_; });
    health_.setActivityLogProvider([this]() -> ActivityLog * { return &activity_log_; });
    profiler_.setRecoveryDirectory(recovery_dir_);
    watchdog_.setSamplerHeartbeat(&profiler_.samplerHeartbeat());
    watchdog_.setAggregatorHeartbeat(&profiler_.aggregatorHeartbeat());
    watchdog_.setStallCallback([this](bool stalled) {
        RecoveryWriter *writer = profiler_.recoveryWriter();
        if (!writer) return;
        const std::uint64_t now = Heartbeat::monotonicNowNs();
        if (stalled) {
            stall_begin_ns_ = now;
            writer->journalStallBegin(now, server_heartbeat_.last_ns.load(std::memory_order_acquire));
            writer->requestFlush();
        } else {
            writer->journalStallEnd(stall_begin_ns_, now);
            writer->requestFlush();
        }
    });
}

void SparkApplication::registerCommands()
{
    registry_.registerCommand(
        {"profiler", "sampler"}, "start/stop/info/cancel/open/trust-viewer an execution or allocation profile",
        "spark.profiler",
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
            else if (action == "open") {
                profiler_.cmdOpen(sender);
            }
            else if (action == "trust-viewer") {
                profiler_.cmdTrustViewer(sender, args);
            }
            else {
                profiler_.cmdInfo(sender);
            }
        });
    registry_.registerCommand(
        {"tps", "cpu"}, "rolling TPS, MSPT percentiles, and CPU usage",
        "spark.tps",
        [this](CommandSender &sender, const Arguments &) {
            health_.cmdTps(sender);
        });
    registry_.registerCommand(
        {"ping"}, "player ping RTT statistics",
        "spark.ping",
        [this](CommandSender &sender, const Arguments &args) {
            health_.cmdPing(sender, args);
        });
    registry_.registerCommand(
        {"health", "healthreport", "ht"}, "performance and host resource report",
        "spark.health",
        [this](CommandSender &sender, const Arguments &args) {
            health_.cmdHealth(sender, args);
        });
    registry_.registerCommand(
        {"activity", "activitylog", "log"}, "show recent profiler and health report activity",
        "spark.activity",
        [this](CommandSender &sender, const Arguments &args) {
            activity_command_.cmdActivity(sender, args);
        });
    registry_.registerCommand(
        {"tickmonitor", "tickmonitoring"}, "report unusually long ticks",
        "spark.tickmonitor",
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
    server_heartbeat_.beat();
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

void SparkApplication::enable()
{
    recoverPreviousSession();
    watchdog_.start();
    profiler_.startBackgroundProfiler();
}

void SparkApplication::shutdown()
{
    profiler_.shutdown();
    watchdog_.stop();
}

void SparkApplication::recoverPreviousSession()
{
    namespace fs = std::filesystem;
    std::error_code ec;

    if (!fs::exists(recovery_dir_, ec) || ec) return;

    // Check for any segment-*.jnl files.
    bool has_journal = false;
    for (const auto &entry : fs::directory_iterator(recovery_dir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto name = entry.path().filename().string();
        if (name.size() >= 8 && name.substr(0, 8) == "segment-" &&
            name.size() >= 4 && name.substr(name.size() - 4) == ".jnl") {
            has_journal = true;
            break;
        }
    }
    if (!has_journal) return;

    RecoveredProfile profile = RecoveryPlayer::replay(recovery_dir_);
    if (!profile.valid) {
        fs::remove_all(recovery_dir_, ec);
        fs::create_directories(recovery_dir_, ec);
        return;
    }

    // Skip recovery for sessions that ended cleanly.  The profiler already
    // exported the profile through the normal path; the journal is just
    // leftover state that should be cleaned up.
    if (profile.has_clean_end) {
        fs::remove_all(recovery_dir_, ec);
        fs::create_directories(recovery_dir_, ec);
        return;
    }

    // Compress and save.
    try {
        std::string compressed = gzipCompress(profile.serialized_proto);
        ProfileFileResult saved = saveProfileToDirectory(
            fs::path(recovery_dir_).parent_path(), compressed, profile.session_start_ms);
        if (saved.ok) {
            notifier_.notify("crash recovery",
                             "Recovered profile saved to " + saved.path.string() +
                             " - open it at " + config_.viewer_url);
        }
    }
    catch (const std::exception &) {
        // Best-effort: delete the journal regardless so we don't loop.
    }

    // Clear the recovery directory for the new session.
    fs::remove_all(recovery_dir_, ec);
    fs::create_directories(recovery_dir_, ec);
}

}  // namespace spark
