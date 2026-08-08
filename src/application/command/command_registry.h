#ifndef SPARK_APPLICATION_COMMAND_COMMAND_REGISTRY_H
#define SPARK_APPLICATION_COMMAND_COMMAND_REGISTRY_H

#include <functional>
#include <string>
#include <vector>

#include "application/command/command_sender.h"
#include "core/command/arguments.h"

namespace spark {

// Registers spark subcommands and dispatches by alias.
// Each command has a primary alias, a one-line description (for help text),
// and a handler that receives the sender and parsed arguments.
class CommandRegistry {
public:
    using Handler = std::function<void(CommandSender &, const Arguments &)>;

    void registerCommand(std::string alias, std::string description, Handler handler);

    // Returns true if a command matched and was executed.
    bool dispatch(CommandSender &sender, const std::vector<std::string> &tokens) const;

    void sendHelp(CommandSender &sender) const;

private:
    struct Entry {
        std::string alias;
        std::string description;
        Handler handler;
    };
    std::vector<Entry> commands_;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_COMMAND_COMMAND_REGISTRY_H
