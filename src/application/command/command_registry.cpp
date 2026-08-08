#include "application/command/command_registry.h"

#include <utility>

#include "core/util/format.h"
#include "spark_constants.h"

namespace spark {

void CommandRegistry::registerCommand(std::string alias, std::string description,
                                       Handler handler)
{
    commands_.push_back({std::move(alias), std::move(description), std::move(handler)});
}

bool CommandRegistry::dispatch(CommandSender &sender,
                                const std::vector<std::string> &tokens) const
{
    if (tokens.empty()) {
        sendHelp(sender);
        return true;
    }
    const std::string &alias = tokens[0];
    for (const auto &cmd : commands_) {
        if (cmd.alias == alias) {
            std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
            cmd.handler(sender, Arguments(rest));
            return true;
        }
    }
    sendHelp(sender);
    return true;
}

void CommandRegistry::sendHelp(CommandSender &sender) const
{
    sender.sendMessage(kColorGold + "endstone-spark " + kColorGray + "v" + spark::kVersion);
    for (const auto &cmd : commands_) {
        sender.sendMessage(kColorYellow + "/spark " + cmd.alias + " " +
                           kColorGray + "- " + cmd.description);
    }
    sender.sendMessage(kColorGray + "Modes: --alloc, --alloc-live-only");
    sender.sendMessage(kColorGray + "Thread selection: --thread <name|*>, --regex");
    sender.sendMessage(kColorGray + "Execution only: --include-sleeping");
    sender.sendMessage(kColorGray + "Flags: --interval <ms|bytes>, --timeout <seconds>, --only-ticks-over <ms>");
    sender.sendMessage(kColorGray + "       --save-to-file (plugins/spark/profiles), --comment <text>");
    sender.sendMessage(kColorGray + "Ping: --player <username>");
}

}  // namespace spark
