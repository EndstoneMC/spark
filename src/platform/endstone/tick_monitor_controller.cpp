#include "platform/endstone/tick_monitor_controller.h"

#include <string>

#include <endstone/endstone.hpp>

#include "core/command/arguments.h"
#include "core/stats/tick_monitor.h"

namespace spark::endstone_adapter {

using endstone::ColorFormat;

TickMonitorController::TickMonitorController(::endstone::Plugin &plugin)
    : plugin_(plugin)
{
}

void TickMonitorController::cmdTickMonitor(::endstone::CommandSender &sender,
                                           const spark::Arguments &args)
{
    if (tick_monitor_.running()) {
        tick_monitor_.stop();
        sender.sendMessage("{}Tick monitor disabled.", ColorFormat::Gold);
        return;
    }

    const bool has_percentage = args.boolFlag("threshold");
    const bool has_duration = args.boolFlag("threshold-tick");
    if (has_percentage && has_duration) {
        sender.sendErrorMessage("Choose either --threshold or --threshold-tick, not both.");
        return;
    }

    spark::TickMonitorConfig config;
    if (has_percentage) {
        auto threshold = args.doubleFlag("threshold");
        if (!threshold || *threshold <= 0.0) {
            sender.sendErrorMessage("The percentage threshold must be a positive number.");
            return;
        }
        config.mode = spark::TickMonitorMode::Percentage;
        config.threshold = *threshold;
    }
    else if (has_duration) {
        auto threshold = args.doubleFlag("threshold-tick");
        if (!threshold || *threshold <= 0.0) {
            sender.sendErrorMessage("The tick duration threshold must be a positive number of milliseconds.");
            return;
        }
        config.mode = spark::TickMonitorMode::Duration;
        config.threshold = *threshold;
    }

    if (!tick_monitor_.start(config)) {
        sender.sendErrorMessage("Unable to start the tick monitor with the requested threshold.");
        return;
    }
    tick_monitor_sender_ = sender.getName();
    sender.sendMessage("{}Tick monitor started.{} Calculating the baseline over 120 ticks (about 6 seconds).",
                       ColorFormat::Gold, ColorFormat::Gray);
}

void TickMonitorController::onTick(double mspt)
{
    if (tick_monitor_.running()) {
        processTickMonitor(mspt);
    }
}

void TickMonitorController::processTickMonitor(double mspt)
{
    spark::TickMonitorUpdate update = tick_monitor_.onTick(mspt);
    char message[256];
    if (update.setup_completed) {
        std::snprintf(message, sizeof(message),
                      "Tick monitor baseline ready: min %.2fms, average %.2fms, max %.2fms.",
                      update.setup_min_ms, update.baseline_ms, update.setup_max_ms);
        announce(tick_monitor_sender_, message);

        if (tick_monitor_.config().mode == spark::TickMonitorMode::Duration) {
            std::snprintf(message, sizeof(message), "Reporting ticks longer than %.2fms.",
                          tick_monitor_.config().threshold);
        }
        else {
            std::snprintf(message, sizeof(message),
                          "Reporting ticks more than %.2f%% above the baseline.",
                          tick_monitor_.config().threshold);
        }
        announce(tick_monitor_sender_, message);
    }
    if (update.report) {
        std::snprintf(message, sizeof(message),
                      "Tick #%llu lasted %.2fms (%.2f%% change from the %.2fms baseline).",
                      static_cast<unsigned long long>(update.tick), update.duration_ms,
                      update.percentage_change, update.baseline_ms);
        announce(tick_monitor_sender_, message);
    }
}

void TickMonitorController::announce(const std::string &sender_name, const std::string &text)
{
    plugin_.getLogger().info("{}", text);
    auto player = plugin_.getServer().getPlayer(sender_name);
    if (player) {
        player->sendMessage("{}[spark] {}{}", ColorFormat::Gold, ColorFormat::Reset, text);
    }
}

}  // namespace spark::endstone_adapter
