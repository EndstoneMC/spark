#include "application/spark_application.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <utility>

#include "application/command/profiler_action_resolver.h"
#include "core/recovery/replay_guard.h"
#include "core/util/monotonic_time.h"
#include "native/sampler/heartbeat.h"
#include "net/profile_file.h"

namespace spark {

namespace {

void logRecoveryFailure(std::string_view disposition = {}) noexcept
{
    if (disposition.empty()) {
        std::fputs("Spark recovery failed; continuing without recovery.\n", stderr);
        return;
    }
    std::fputs("Spark recovery notification failed; ", stderr);
    std::fwrite(disposition.data(), 1, disposition.size(), stderr);
    std::fputc('\n', stderr);
}

}  // namespace

SparkApplication::SparkApplication(std::string bds_executable_sha256, const std::filesystem::path &profile_storage_dir,
                                   std::filesystem::path activity_log_file, SparkConfig config,
                                   TrustedViewersState trusted_viewers, MainThreadDispatcher &dispatcher,
                                   ProfileMetadataProvider &metadata_provider, ResultNotifier &notifier)
    : config_(std::move(config)), trusted_viewers_(std::move(trusted_viewers)), dispatcher_(dispatcher),
      metadata_provider_(metadata_provider), notifier_(notifier),
      profiler_(statistics_, std::move(bds_executable_sha256), profile_storage_dir, config_.bytebin_url,
                config_.viewer_url, config_.bytesocks_host, config_.background_profiler_enabled,
                config_.background_profiler_interval, config_.background_profiler_thread_grouper,
                config_.background_profiler_thread_dumper, trusted_viewers_, dispatcher_, metadata_provider_,
                notifier_),
      health_(statistics_, metadata_provider_, config_.bytebin_url, config_.viewer_url, config_.bytesocks_host,
              trusted_viewers_, dispatcher_, notifier_),
      activity_log_(std::move(activity_log_file)), activity_command_(activity_log_), tick_monitor_(notifier_),
      watchdog_(server_heartbeat_)
{
    ci_diagnostics_.open();
    recovery_dir_ = profile_storage_dir / "recovery";
    profiler_.configureAllocationRateMetrics(config_.allocation_rate_metrics_enabled);
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
        const std::uint64_t now = Heartbeat::monotonicNowNs();
        if (stalled) {
            stall_begin_ns_ = now;
            profiler_.journalStallBegin(now, server_heartbeat_.last_ns.load(std::memory_order_acquire));
        }
        else {
            profiler_.journalStallEnd(stall_begin_ns_, now);
        }
    });
}

void SparkApplication::registerCommands()
{
    registry_.registerCommand({"profiler", "sampler"},
                              "start/stop/info/cancel/open/trust-viewer an execution or allocation profile",
                              "spark.profiler", true, [this](CommandSender &sender, const Arguments &args) {
                                  switch (resolveProfilerAction(args)) {
                                  case ProfilerAction::Info:
                                      profiler_.cmdInfo(sender);
                                      break;
                                  case ProfilerAction::Open:
                                      profiler_.cmdOpen(sender, args);
                                      break;
                                  case ProfilerAction::TrustViewer:
                                      profiler_.cmdTrustViewer(sender, args);
                                      break;
                                  case ProfilerAction::Cancel:
                                      profiler_.cmdCancel(sender);
                                      break;
                                  case ProfilerAction::Stop:
                                      profiler_.cmdStop(sender, args);
                                      break;
                                  case ProfilerAction::Start:
                                      profiler_.cmdStart(sender, args);
                                      break;
                                  }
                              });
    registry_.registerCommand({"tps", "cpu"}, "rolling TPS, MSPT percentiles, and CPU usage", "spark.tps", false,
                              [this](CommandSender &sender, const Arguments &) { health_.cmdTps(sender); });
    registry_.registerCommand({"ping"}, "player ping RTT statistics", "spark.ping", false,
                              [this](CommandSender &sender, const Arguments &args) { health_.cmdPing(sender, args); });
    registry_.registerCommand(
        {"health", "healthreport", "ht"}, "show, upload, or open the health dashboard", "spark.health", true,
        [this](CommandSender &sender, const Arguments &args) { health_.cmdHealth(sender, args); });
    registry_.registerCommand(
        {"activity", "activitylog", "log"}, "show recent profiler and health report activity", "spark.activity", false,
        [this](CommandSender &sender, const Arguments &args) { activity_command_.cmdActivity(sender, args); });
    registry_.registerCommand(
        {"tickmonitor", "tickmonitoring"}, "report unusually long ticks", "spark.tickmonitor", false,
        [this](CommandSender &sender, const Arguments &args) { tick_monitor_.cmdTickMonitor(sender, args); });
}

bool SparkApplication::dispatchCommand(CommandSender &sender, const std::vector<std::string> &tokens)
{
    CiDiagnostics::Scope diagnostic_scope(
        globalCiDiagnostics(), CiDiagnosticContext::ApplicationCommand, CiDiagnosticPhase::ApplicationCommandEnter,
        CiDiagnosticPhase::ApplicationCommandExit, CiDiagnosticPhase::ApplicationCommandExceptionalExit,
        ciDiagnosticCurrentThreadId());
    const bool handled = registry_.dispatch(sender, tokens);
    return handled;
}

void SparkApplication::onTick(double mspt)
{
    CiDiagnostics::Scope diagnostic_scope(
        globalCiDiagnostics(), CiDiagnosticContext::ApplicationTick, CiDiagnosticPhase::ApplicationTickEnter,
        CiDiagnosticPhase::ApplicationTickExit, CiDiagnosticPhase::ApplicationTickExceptionalExit,
        ciDiagnosticCurrentThreadId());
    server_heartbeat_.beat();
    if (profiler_.allocationRateMetricsActive()) {
        statistics_.recordAllocationBytes(profiler_.persistentAllocationBytes());
    }
    if (statistics_.onTick(mspt)) {
        statistics_.recordPlayerCount(metadata_provider_.playerCount());
        const WorldGaugeValues gauges = metadata_provider_.worldGauges();
        statistics_.recordWorldGauges(gauges.entities, gauges.tile_entities, gauges.chunks,
                                      gauges.tile_entities_present);
    }
    const MonitoringDue monitoring_due = monitoring_schedule_.poll(monotonicUnixMillis());
    if (monitoring_due.ping) {
        health_.pollPing();
    }
    if (monitoring_due.network) {
        health_.pollNetwork();
    }
    health_.onTick();
    tick_monitor_.onTick(mspt);
    profiler_.onTick(mspt);
    surfaceJournalDegradation();
}

void SparkApplication::enable()
{
    recoverPreviousSession();
    watchdog_.start();
    profiler_.startBackgroundProfiler();
}

bool SparkApplication::shutdown(std::string &error)
{
    error.clear();
    if (!health_.shutdownWithin(std::chrono::seconds(2))) {
        error = "health dashboard/upload shutdown timed out";
        return false;
    }
    if (!profiler_.shutdown(error)) {
        if (error.empty()) {
            error = "profiler shutdown failed";
        }
        return false;
    }
    watchdog_.stop();
    return true;
}

void SparkApplication::shutdown()
{
    std::string error;
    if (shutdown(error)) {
        return;
    }
    health_.shutdown();
    profiler_.shutdown();
    watchdog_.stop();
}

void SparkApplication::recoverPreviousSession() noexcept
{
    try {
        recoverPreviousSessionImpl();
    }
    catch (...) {
        logRecoveryFailure();
    }
}

void SparkApplication::recoverPreviousSessionImpl()
{
    namespace fs = std::filesystem;
    std::error_code ec;

    const auto publish_terminal_guard = [this] {
        auto publish_error = recoveryTestFailure("guard-publish", recovery_dir_);
        if (!publish_error && blockRecoveryReplay(recovery_dir_, publish_error)) {
            return true;
        }
        profiler_.setRecoveryDirectory({});
        std::string message =
            "Could not persist the replay guard. The recovery journal was retained and recovery writing is disabled "
            "for this session; replay may happen again after restart.";
        if (publish_error) {
            message += " ";
            message += publish_error.message();
        }
        safeNotify("crash recovery", message, "replay block was not persisted; journal retained");
        return false;
    };

    const auto retire_terminal_guard = [this](std::error_code &error) {
        error = recoveryTestFailure("guard-clear", recovery_dir_);
        return !error && clearRecoveryReplayBlock(recovery_dir_, error);
    };

    const auto finish_terminal_cleanup = [this, &retire_terminal_guard](const RecoveryCleanupResult &cleanup) {
        if (!cleanup.old_generation_removed) {
            profiler_.setRecoveryDirectory({});
            std::string message =
                "Old recovery artifacts remain; automatic replay is blocked and recovery writing is disabled for "
                "this session.";
            if (cleanup.error) {
                message += " ";
                message += cleanup.error.message();
            }
            safeNotify("crash recovery", message, "recovery artifacts remain; replay is blocked");
            return;
        }

        std::error_code error;
        if (!retire_terminal_guard(error)) {
            profiler_.setRecoveryDirectory({});
            std::string message =
                "Recovery artifacts were removed, but the replay guard could not be retired; recovery writing is "
                "disabled for this session.";
            if (error) {
                message += " ";
                message += error.message();
            }
            safeNotify("crash recovery", message, "replay guard remains set; recovery writing is disabled");
            return;
        }
        if (!cleanup.fresh_directory_available) {
            profiler_.setRecoveryDirectory({});
            std::string message =
                "Recovery artifacts were removed, but a fresh recovery directory could not be created; recovery "
                "writing is disabled for this session.";
            if (cleanup.error) {
                message += " ";
                message += cleanup.error.message();
            }
            safeNotify("crash recovery", message, "recovery journal was purged; fresh recovery directory unavailable");
        }
    };

    ec = recoveryTestFailure("guard-probe", recovery_dir_);
    const bool guard_probe_failed = static_cast<bool>(ec);
    const bool replay_blocked = guard_probe_failed || recoveryReplayBlocked(recovery_dir_, ec);
    if (guard_probe_failed || ec) {
        profiler_.setRecoveryDirectory({});
        std::string message =
            "The replay guard could not be inspected; automatic replay was skipped and recovery writing is disabled "
            "for this session.";
        if (ec) {
            message += " ";
            message += ec.message();
        }
        safeNotify("crash recovery", message, "replay guard state is unknown; replay skipped");
        safeNotify("crash recovery",
                   "Recovery artifacts may remain; no cleanup was attempted because guard state "
                   "is unknown.",
                   "recovery artifacts may remain; replay guard state is unknown");
        return;
    }

    if (replay_blocked) {
        safeNotify("crash recovery", "Automatic replay is blocked for the retained recovery directory.",
                   "automatic replay remains blocked");
        const auto cleanup = discardJournal();
        finish_terminal_cleanup(cleanup);
        return;
    }
    if (!fs::exists(recovery_dir_, ec) || ec) {
        if (ec) {
            profiler_.setRecoveryDirectory({});
            safeNotify("crash recovery",
                       "The recovery directory could not be inspected; automatic replay was skipped and recovery "
                       "writing is disabled for this session.",
                       "recovery directory unavailable; replay skipped");
        }
        return;
    }

    // Check for any segment-*.jnl files.
    bool has_journal = false;
    fs::directory_iterator entries(recovery_dir_, ec);
    if (ec) {
        profiler_.setRecoveryDirectory({});
        safeNotify("crash recovery",
                   "The recovery directory could not be inspected; automatic replay was skipped and recovery writing "
                   "is disabled for this session.",
                   "recovery directory unavailable; replay skipped");
        return;
    }
    const fs::directory_iterator end;
    for (; entries != end; entries.increment(ec)) {
        if (ec) {
            break;
        }
        const auto status = entries->status(ec);
        if (ec) {
            break;
        }
        if (status.type() != fs::file_type::regular) {
            continue;
        }
        const std::string name = entries->path().filename().string();
        if (name.starts_with("segment-") && name.ends_with(".jnl")) {
            has_journal = true;
            break;
        }
    }
    if (ec) {
        profiler_.setRecoveryDirectory({});
        safeNotify("crash recovery",
                   "The recovery directory could not be inspected; automatic replay was skipped and recovery writing "
                   "is disabled for this session.",
                   "recovery directory unavailable; replay skipped");
        return;
    }
    if (!has_journal) {
        return;
    }

    RecoveredProfile profile;
    try {
        if (const auto injected = recoveryTestFailure("replay", recovery_dir_); injected) {
            throw fs::filesystem_error("injected recovery replay failure", recovery_dir_, injected);
        }
        profile = RecoveryPlayer::replay(recovery_dir_);
    }
    catch (const std::exception &e) {
        if (publish_terminal_guard()) {
            finish_terminal_cleanup(quarantineRecovery(std::string("replay exception: ") + e.what()));
        }
        return;
    }
    catch (...) {
        if (publish_terminal_guard()) {
            finish_terminal_cleanup(quarantineRecovery("replay exception: unknown error"));
        }
        return;
    }

    if (!profile.valid) {
        if (!publish_terminal_guard()) {
            return;
        }
        safeNotify("crash recovery", "Discarding incomplete recovery journal: " + profile.error,
                   "invalid recovery journal reached terminal cleanup");
        auto cleanup = discardJournal();
        if (!cleanup.old_generation_removed) {
            cleanup = quarantineRecovery("could not discard recovery journal");
        }
        finish_terminal_cleanup(cleanup);
        return;
    }

    // Skip recovery for sessions that ended cleanly (old-format journals
    // that carry a CleanEnd marker).  The journal is just leftover state.
    if (profile.has_clean_end) {
        if (!publish_terminal_guard()) {
            return;
        }
        auto cleanup = discardJournal();
        if (!cleanup.old_generation_removed) {
            cleanup = quarantineRecovery("could not discard recovery journal");
        }
        finish_terminal_cleanup(cleanup);
        return;
    }

    bool saved_profile = false;
    try {
        ProfileFileResult saved = saveProfileToDirectory(fs::path(recovery_dir_).parent_path(),
                                                         profile.serialized_proto, profile.session_start_ms);
        if (saved.ok) {
            saved_profile = true;
            safeNotify("crash recovery",
                       "Recovered profile saved to " + saved.path.string() + " - open it at " + config_.viewer_url,
                       "recovered profile saved; journal cleanup follows");
        }
        else {
            safeNotify("crash recovery", "Failed to save recovered profile; recovery journal retained: " + saved.error,
                       "recovery journal retained for retry after save failure");
        }
    }
    catch (const std::exception &error) {
        safeNotify("crash recovery",
                   "Failed to save recovered profile; recovery journal retained: " + std::string(error.what()),
                   "recovery journal retained for retry after save failure");
    }
    catch (...) {
        safeNotify("crash recovery", "Failed to save recovered profile; recovery journal retained",
                   "recovery journal retained for retry after save failure");
    }

    if (saved_profile) {
        if (!publish_terminal_guard()) {
            return;
        }
        auto cleanup = discardJournal();
        if (!cleanup.old_generation_removed) {
            cleanup = quarantineRecovery("could not discard recovery journal");
        }
        finish_terminal_cleanup(cleanup);
    }
    else {
        profiler_.setRecoveryDirectory({});
    }
}

SparkApplication::RecoveryCleanupResult SparkApplication::discardJournal() noexcept
{
    namespace fs = std::filesystem;
    RecoveryCleanupResult result;
    try {
        result.error = recoveryTestFailure("discard-remove", recovery_dir_);
        if (result.error) {
            return result;
        }
        fs::remove_all(recovery_dir_, result.error);
        if (result.error) {
            return result;
        }
        result.old_generation_removed = true;

        result.error = recoveryTestFailure("create-directory", recovery_dir_);
        if (result.error) {
            return result;
        }
        fs::create_directories(recovery_dir_, result.error);
        result.fresh_directory_available = !result.error;
        return result;
    }
    catch (...) {
        result.error = std::make_error_code(std::errc::io_error);
        return result;
    }
}

SparkApplication::RecoveryCleanupResult SparkApplication::quarantineRecovery(const std::string &reason)
{
    namespace fs = std::filesystem;
    RecoveryCleanupResult result;
    try {
        safeNotify("crash recovery",
                   "Quarantining recovery journal (" + reason + "). The server will continue starting up normally.",
                   "recovery journal quarantine or removal was attempted");

        // Collision-resistant naming: same-second quarantines get a monotonic suffix.
        const auto stamp =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
        const auto quarantined = recovery_dir_.parent_path() / ("recovery.failed-" + std::to_string(stamp.count()) +
                                                                "-" + std::to_string(quarantine_counter_++));
        result.error = recoveryTestFailure("quarantine-rename", recovery_dir_, quarantined);
        if (!result.error) {
            fs::rename(recovery_dir_, quarantined, result.error);
            result.old_generation_removed = !result.error;
        }
        if (!result.old_generation_removed) {
            result.error = recoveryTestFailure("quarantine-remove", recovery_dir_);
            if (!result.error) {
                fs::remove_all(recovery_dir_, result.error);
                result.old_generation_removed = !result.error;
            }
        }

        const auto create_error = recoveryTestFailure("create-directory", recovery_dir_);
        if (create_error) {
            if (!result.error) {
                result.error = create_error;
            }
            return result;
        }
        std::error_code create_ec;
        fs::create_directories(recovery_dir_, create_ec);
        if (create_ec) {
            if (!result.error) {
                result.error = create_ec;
            }
            return result;
        }
        result.fresh_directory_available = true;
        return result;
    }
    catch (...) {
        if (!result.error) {
            result.error = std::make_error_code(std::errc::io_error);
        }
        return result;
    }
}

std::error_code SparkApplication::recoveryTestFailure(std::string_view operation, const std::filesystem::path &path,
                                                      const std::filesystem::path &other) const noexcept
{
    if (!recovery_test_hook_) {
        return {};
    }
    try {
        return recovery_test_hook_(operation, path, other);
    }
    catch (...) {
        return std::make_error_code(std::errc::io_error);
    }
}

void SparkApplication::surfaceJournalDegradation() noexcept
{
    try {
        std::string cause;
        if (profiler_.reportJournalDegradationIfNeeded(cause)) {
            safeNotify("crash recovery",
                       "Crash-recovery journal degraded (" + cause + "); this session may not be recoverable.");
        }
    }
    catch (...) {
        logRecoveryFailure();
    }
}

void SparkApplication::safeNotify(const std::string &sender, const std::string &message,
                                  std::string_view fallback_disposition) noexcept
{
    try {
        notifier_.notify(sender, message);
    }
    catch (...) {
        logRecoveryFailure(fallback_disposition);
    }
}

}  // namespace spark
