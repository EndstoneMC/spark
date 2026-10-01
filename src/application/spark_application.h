#ifndef SPARK_APPLICATION_SPARK_APPLICATION_H
#define SPARK_APPLICATION_SPARK_APPLICATION_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "application/activity/activity_command.h"
#include "application/command/command_registry.h"
#include "application/command/command_sender.h"
#include "application/health/health_command.h"
#include "application/monitoring/monitoring_schedule.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profiler_service.h"
#include "application/tick_monitor/tick_monitor_command.h"
#include "core/activity/activity_log.h"
#include "core/config/spark_config.h"
#include "core/config/trusted_viewers.h"
#include "core/recovery/recovery_player.h"
#include "core/recovery/stall_watchdog.h"
#include "core/stats/statistics_service.h"
#include "native/diagnostics/ci_diagnostics.h"

namespace spark {

// Central application container. Owns all platform-independent services
// and wires them to platform capabilities injected by the bootstrap.
// The Endstone plugin creates this object and delegates commands and ticks to it.
class SparkApplication {
public:
    SparkApplication(std::string bds_executable_sha256, const std::filesystem::path &profile_storage_dir,
                     std::filesystem::path activity_log_file, SparkConfig config, TrustedViewersState trusted_viewers,
                     MainThreadDispatcher &dispatcher, ProfileMetadataProvider &metadata_provider,
                     ResultNotifier &notifier);

    // Dispatches a /spark command. Returns true if handled.
    bool dispatchCommand(CommandSender &sender, const std::vector<std::string> &tokens);

    // Called every server tick.
    void onTick(double mspt);

    // Sets the server main thread ID (identified lazily).
    void setMainThreadId(std::uint64_t tid) { profiler_.setMainThreadId(tid); }
    void setPythonStackProvider(PythonStackProvider *provider) noexcept { profiler_.setPythonStackProvider(provider); }
    [[nodiscard]] bool executionProfilerRunning() const { return profiler_.executionProfiling(); }
    [[nodiscard]] bool profilerExporting() const { return profiler_.exporting(); }

    // Lifecycle.
    void shutdown();
    bool shutdown(std::string &error);
    void enable();
    bool shutdownProfilerBackend(std::string &error) { return profiler_.shutdownBackend(error); }

    StatisticsService &statistics() { return statistics_; }
    CommandRegistry &registry() { return registry_; }
    HealthCommand &health() { return health_; }
    ActivityLog &activityLog() { return activity_log_; }
    SparkConfig &config() { return config_; }
    const SparkConfig &config() const { return config_; }

private:
    friend struct SparkApplicationRecoveryTestAccess;

    using RecoveryTestHook =
        std::function<std::error_code(std::string_view, const std::filesystem::path &, const std::filesystem::path &)>;

    struct RecoveryCleanupResult {
        bool old_generation_removed = false;
        bool fresh_directory_available = false;
        std::error_code error;
    };

    void registerCommands();
    void recoverPreviousSession() noexcept;
    void recoverPreviousSessionImpl();
    RecoveryCleanupResult quarantineRecovery(const std::string &reason);
    RecoveryCleanupResult discardJournal() noexcept;
    std::error_code recoveryTestFailure(std::string_view operation, const std::filesystem::path &path,
                                        const std::filesystem::path &other = {}) const noexcept;
    void surfaceJournalDegradation() noexcept;
    void safeNotify(const std::string &sender, const std::string &message,
                    std::string_view fallback_disposition = {}) noexcept;

    StatisticsService statistics_;
    SparkConfig config_;
    TrustedViewersState trusted_viewers_;
    MainThreadDispatcher &dispatcher_;
    ProfileMetadataProvider &metadata_provider_;
    ResultNotifier &notifier_;

    // Declared before profiler_ so the mapping outlives all sampler workers.
    CiDiagnostics ci_diagnostics_;
    ProfilerService profiler_;
    HealthCommand health_;
    ActivityLog activity_log_;
    ActivityCommand activity_command_;
    TickMonitorCommand tick_monitor_;
    CommandRegistry registry_;
    MonitoringSchedule monitoring_schedule_;

    // Stall detection: server heartbeat updated every tick, watchdog runs
    // on its own thread and never calls Endstone APIs.
    Heartbeat server_heartbeat_;
    StallWatchdog watchdog_;
    std::uint64_t stall_begin_ns_ = 0;
    std::filesystem::path recovery_dir_;
    std::uint64_t quarantine_counter_ = 0;
    RecoveryTestHook recovery_test_hook_;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_SPARK_APPLICATION_H
