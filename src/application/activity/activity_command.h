#ifndef SPARK_APPLICATION_ACTIVITY_ACTIVITY_COMMAND_H
#define SPARK_APPLICATION_ACTIVITY_ACTIVITY_COMMAND_H

#include "application/command/command_sender.h"
#include "core/activity/activity_log.h"
#include "core/command/arguments.h"

namespace spark {

// Handles /spark activity - displays the persisted activity log.
class ActivityCommand {
public:
    explicit ActivityCommand(ActivityLog &log) : log_(log) {}

    void cmdActivity(CommandSender &sender, const Arguments &args);

private:
    ActivityLog &log_;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_ACTIVITY_ACTIVITY_COMMAND_H
