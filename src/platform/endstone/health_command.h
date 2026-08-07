#ifndef SPARK_PLATFORM_ENDSTONE_HEALTH_COMMAND_H
#define SPARK_PLATFORM_ENDSTONE_HEALTH_COMMAND_H

#include <string>

namespace endstone {
class CommandSender;
class Plugin;
}  // namespace endstone

namespace spark {
class StatisticsService;
struct StatisticsSnapshot;
}  // namespace spark

namespace spark::endstone_adapter {

// Handles /spark tps and /spark health commands.
class HealthCommands {
public:
    HealthCommands(::endstone::Plugin &plugin, spark::StatisticsService &statistics);

    void cmdTps(::endstone::CommandSender &sender);
    void cmdHealth(::endstone::CommandSender &sender);

private:
    void sendPerformanceReport(::endstone::CommandSender &sender,
                               const spark::StatisticsSnapshot &stats);

    ::endstone::Plugin &plugin_;
    spark::StatisticsService &statistics_;
};

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_HEALTH_COMMAND_H
