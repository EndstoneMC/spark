#ifndef SPARK_APPLICATION_TICK_MONITOR_TICK_MONITOR_COMMAND_H
#define SPARK_APPLICATION_TICK_MONITOR_TICK_MONITOR_COMMAND_H

#include <cstdio>
#include <string>

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"
#include "core/command/arguments.h"
#include "core/stats/tick_monitor.h"

namespace spark {

// Handles /spark tickmonitor command and per-tick monitoring.
// Platform-independent: uses CommandSender and ResultNotifier.
class TickMonitorCommand {
public:
    TickMonitorCommand(ResultNotifier &notifier);

    void cmdTickMonitor(CommandSender &sender, const Arguments &args);
    void onTick(double mspt);

    bool running() const { return tick_monitor_.running(); }

private:
    void processTickMonitor(double mspt);

    ResultNotifier &notifier_;
    TickMonitor tick_monitor_;
    std::string sender_name_ = "CONSOLE";
};

}  // namespace spark

#endif  // SPARK_APPLICATION_TICK_MONITOR_TICK_MONITOR_COMMAND_H
