#ifndef SPARK_APPLICATION_SPARK_APPLICATION_H
#define SPARK_APPLICATION_SPARK_APPLICATION_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "application/command/command_registry.h"
#include "application/command/command_sender.h"
#include "application/health/health_command.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profiler_service.h"
#include "application/tick_monitor/tick_monitor_command.h"
#include "core/stats/statistics_service.h"

namespace spark {

// Central application container. Owns all platform-independent services
// and wires them to platform capabilities injected by the bootstrap.
// The Endstone plugin creates this object and delegates commands and ticks to it.
class SparkApplication {
public:
    SparkApplication(std::string bds_executable_sha256,
                     std::filesystem::path profile_storage_dir,
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
    bool shutdownProfilerBackend(std::string &error) { return profiler_.shutdownBackend(error); }

    StatisticsService &statistics() { return statistics_; }
    CommandRegistry &registry() { return registry_; }
    HealthCommand &health() { return health_; }

private:
    void registerCommands();

    StatisticsService statistics_;
    MainThreadDispatcher &dispatcher_;
    ProfileMetadataProvider &metadata_provider_;
    ResultNotifier &notifier_;

    ProfilerService profiler_;
    HealthCommand health_;
    TickMonitorCommand tick_monitor_;
    CommandRegistry registry_;
    std::uint64_t tick_counter_ = 0;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_SPARK_APPLICATION_H
