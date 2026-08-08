#ifndef SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H
#define SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"
#include "core/stats/statistics_service.h"

namespace spark {

// Handles /spark tps and /spark health commands.
// Platform-independent: uses CommandSender and ProfileMetadataProvider.
class HealthCommand {
public:
    HealthCommand(StatisticsService &statistics, ProfileMetadataProvider &metadata_provider);

    void cmdTps(CommandSender &sender);
    void cmdHealth(CommandSender &sender);

private:
    void sendPerformanceReport(CommandSender &sender, const StatisticsSnapshot &stats);

    StatisticsService &statistics_;
    ProfileMetadataProvider &metadata_provider_;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H
