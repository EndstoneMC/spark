#ifndef SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H
#define SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H

#include <memory>

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"
#include "core/command/arguments.h"
#include "core/stats/network_monitor.h"
#include "core/stats/ping_statistics.h"
#include "core/stats/statistics_service.h"

namespace spark {

// Handles /spark tps, /spark ping, and /spark health commands.
// Platform-independent: uses CommandSender and ProfileMetadataProvider.
class HealthCommand {
public:
    HealthCommand(StatisticsService &statistics, ProfileMetadataProvider &metadata_provider);

    void cmdTps(CommandSender &sender);
    void cmdPing(CommandSender &sender, const Arguments &args);
    void cmdHealth(CommandSender &sender);

    // Called periodically (every ~10 seconds) to poll ping data.
    void pollPing();

    // Called periodically (every ~60 seconds) to poll network interface stats.
    void pollNetwork();

    // Returns the current ping samples for profile export, or an empty vector
    // if ping monitoring is not active.
    std::vector<int> pingSamples() const;

    // Returns the current network interface snapshots for profile export.
    std::map<std::string, NetworkInterfaceSnapshot> networkSnapshots() const;

private:
    void sendPerformanceReport(CommandSender &sender, const StatisticsSnapshot &stats);

    StatisticsService &statistics_;
    ProfileMetadataProvider &metadata_provider_;
    std::unique_ptr<PingStatistics> ping_statistics_;
    NetworkMonitor network_monitor_;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H
