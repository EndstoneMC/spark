#ifndef SPARK_APPLICATION_SPARK_APPLICATION_H
#define SPARK_APPLICATION_SPARK_APPLICATION_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "application/activity/activity_command.h"
#include "application/command/command_registry.h"
#include "application/command/command_sender.h"
#include "application/health/health_command.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profiler_service.h"
#include "application/tick_monitor/tick_monitor_command.h"
#include "core/activity/activity_log.h"
#include "core/config/spark_config.h"
#include "core/stats/statistics_service.h"

namespace spark {

// Central application container. Owns all platform-independent services
// and wires them to platform capabilities injected by the bootstrap.
// The Endstone plugin creates this object and delegates commands and ticks to it.
class SparkApplication {
public:
    SparkApplication(std::string bds_executable_sha256,
                     std::filesystem::path profile_storage_dir,
                     std::filesystem::path activity_log_file,
                     SparkConfig config,
                     MainThreadDispatcher &dispatcher,
                     ProfileMetadataProvider &metadata_provider,
                     ResultNotifier &notifier);

    // Dispatches a /spark command. Returns true if handled.
    bool dispatchCommand(CommandSender &sender, const std::vector<std::string> &tokens);

    // Called every server tick.
    void onTick(double mspt);

    // Sets the server main thread ID (identified lazily).
    void setMainThreadId(std::uint64_t tid) { profiler_.setMainThreadId(tid); }

    // Lifecycle.
    void shutdown();
    void enable();
    bool shutdownProfilerBackend(std::string &error) { return profiler_.shutdownBackend(error); }

    StatisticsService &statistics() { return statistics_; }
    CommandRegistry &registry() { return registry_; }
    HealthCommand &health() { return health_; }
    ActivityLog &activityLog() { return activity_log_; }
    SparkConfig &config() { return config_; }
    const SparkConfig &config() const { return config_; }

private:
    void registerCommands();

    StatisticsService statistics_;
    SparkConfig config_;
    MainThreadDispatcher &dispatcher_;
    ProfileMetadataProvider &metadata_provider_;
    ResultNotifier &notifier_;

    ProfilerService profiler_;
    HealthCommand health_;
    ActivityLog activity_log_;
    ActivityCommand activity_command_;
    TickMonitorCommand tick_monitor_;
    CommandRegistry registry_;
    std::uint64_t tick_counter_ = 0;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_SPARK_APPLICATION_H
