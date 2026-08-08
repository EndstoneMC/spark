#include "application/command/command_registry.h"

#include <exception>
#include <utility>

#include "core/util/format.h"
#include "spark_constants.h"

namespace spark {

void CommandRegistry::registerCommand(std::vector<std::string> aliases, std::string description, std::string permission,
                                      Handler handler)
{
    commands_.push_back({.aliases = std::move(aliases),
                         .description = std::move(description),
                         .permission = std::move(permission),
                         .handler = std::move(handler)});
}

bool CommandRegistry::dispatch(CommandSender &sender, const std::vector<std::string> &tokens) const
{
    if (tokens.empty()) {
        sendHelp(sender);
        return true;
    }
    const std::string &alias = tokens[0];
    for (const auto &cmd : commands_) {
        for (const auto &a : cmd.aliases) {
            if (a == alias) {
                if (!cmd.permission.empty() && !sender.hasPermission(cmd.permission)) {
                    sender.sendErrorMessage("You do not have permission to use this command.");
                    return true;
                }
                std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
                try {
                    cmd.handler(sender, Arguments(rest));
                }
                catch (const std::exception &e) {
                    sender.sendErrorMessage("Internal error: {}", e.what());
                }
                catch (...) {
                    sender.sendErrorMessage("Internal error: unknown exception");
                }
                return true;
            }
        }
    }
    sendHelp(sender);
    return true;
}

void CommandRegistry::sendHelp(CommandSender &sender) const
{
    sender.sendMessage(kColorGold + "endstone-spark " + kColorGray + "v" + spark::kVersion);
    for (const auto &cmd : commands_) {
        std::string message = kColorYellow + "/spark ";
        message += cmd.aliases[0] + " " + kColorGray + "- " + cmd.description;
        sender.sendMessage(message);
    }
    sender.sendMessage(kColorGray + "Modes: --alloc, --alloc-live-only");
    sender.sendMessage(kColorGray + "Thread selection: --thread <name|*>, --regex");
    sender.sendMessage(kColorGray + "Thread grouping: --not-combined (separate), --combine-all (merge all)");
    sender.sendMessage(kColorGray + "Execution only: --ignore-sleeping");
    sender.sendMessage(kColorGray + "Flags: --interval <ms|bytes>, --timeout <seconds>, --only-ticks-over <ms>");
    sender.sendMessage(kColorGray + "       --save-to-file (plugins/spark/profiles), --comment <text>");
    sender.sendMessage(kColorGray + "Ping: --player <username>");
    sender.sendMessage(kColorGray + "Health: --upload (upload a health report to the spark viewer)");
    sender.sendMessage(kColorGray + "Activity: --page <number>");
}

}  // namespace spark
