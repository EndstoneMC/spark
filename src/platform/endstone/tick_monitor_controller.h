#ifndef SPARK_PLATFORM_ENDSTONE_TICK_MONITOR_CONTROLLER_H
#define SPARK_PLATFORM_ENDSTONE_TICK_MONITOR_CONTROLLER_H

#include <cstdio>
#include <string>

#include "stats/tick_monitor.h"

namespace endstone {
class CommandSender;
class Plugin;
}  // namespace endstone

namespace spark {
class Arguments;
}  // namespace spark

namespace spark::endstone_adapter {

// Handles /spark tickmonitor command and per-tick monitoring.
class TickMonitorController {
public:
    TickMonitorController(::endstone::Plugin &plugin);

    void cmdTickMonitor(::endstone::CommandSender &sender, const spark::Arguments &args);
    void onTick(double mspt);

    bool running() const { return tick_monitor_.running(); }

private:
    void processTickMonitor(double mspt);
    void announce(const std::string &sender_name, const std::string &text);

    ::endstone::Plugin &plugin_;
    spark::TickMonitor tick_monitor_;
    std::string tick_monitor_sender_ = "CONSOLE";
};

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_TICK_MONITOR_CONTROLLER_H
