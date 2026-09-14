#include "application/command/command_registry.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

#include "core/util/format.h"
#include "spark_constants.h"

namespace spark {
namespace {

constexpr char KNativeSparkPermission[] = "endstone.command.spark";
constexpr char KJavaSparkPermission[] = "spark";

std::string lowerCase(std::string value)
{
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool canUse(const CommandSender &sender, const std::string &permission)
{
    return permission.empty() || sender.hasPermission(permission) || sender.hasPermission(KNativeSparkPermission) ||
           sender.hasPermission(KJavaSparkPermission);
}

}  // namespace

void CommandRegistry::registerCommand(std::vector<std::string> aliases, std::string description, std::string permission,
                                      bool allow_subcommand, Handler handler)
{
    for (std::string &alias : aliases) {
        alias = lowerCase(std::move(alias));
    }
    commands_.push_back({.aliases = std::move(aliases),
                         .description = std::move(description),
                         .permission = std::move(permission),
                         .allow_subcommand = allow_subcommand,
                         .handler = std::move(handler)});
}

bool CommandRegistry::dispatch(CommandSender &sender, const std::vector<std::string> &tokens) const
{
    if (tokens.empty()) {
        sendHelp(sender);
        return true;
    }
    const std::string alias = lowerCase(tokens[0]);
    for (const auto &cmd : commands_) {
        for (const auto &a : cmd.aliases) {
            if (a == alias) {
                if (!canUse(sender, cmd.permission)) {
                    sender.sendErrorMessage("You do not have permission to use this command.");
                    return true;
                }
                std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
                try {
                    cmd.handler(sender, Arguments(rest, cmd.allow_subcommand));
                }
                catch (const Arguments::ParseError &e) {
                    sender.sendErrorMessage(e.what());
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
    const auto can_use = [this, &sender](const std::string &alias) {
        for (const auto &command : commands_) {
            if (std::ranges::find(command.aliases, alias) != command.aliases.end()) {
                return canUse(sender, command.permission);
            }
        }
        return false;
    };

    sender.sendMessage(kColorGold + "endstone-spark " + kColorGray + "v" + spark::kVersion);
    for (const auto &cmd : commands_) {
        if (!canUse(sender, cmd.permission)) {
            continue;
        }
        std::string message = kColorYellow + "/spark ";
        message += cmd.aliases[0] + " " + kColorGray + "- " + cmd.description;
        sender.sendMessage(message);
    }
    if (can_use("profiler")) {
        sender.sendMessage(kColorGray + "Modes: --alloc, --alloc-live-only");
        sender.sendMessage(kColorGray + "Thread selection: --thread <name|*>, --regex");
        sender.sendMessage(kColorGray + "Thread grouping: --not-combined (separate), --combine-all (merge all)");
        sender.sendMessage(kColorGray + "Execution only: --ignore-sleeping");
        sender.sendMessage(kColorGray + "Flags: --interval <ms|bytes>, --timeout <seconds>, --only-ticks-over <ms>");
        sender.sendMessage(kColorGray + "       --save-to-file (plugins/spark/profiles), --comment <text>");
    }
    if (can_use("ping")) {
        sender.sendMessage(kColorGray + "Ping: --player <username>");
    }
    if (can_use("health")) {
        sender.sendMessage(kColorGray + "Health: show/upload/dashboard/trust-viewer [--id <client id>]");
        sender.sendMessage(kColorGray + "       health show: --memory (process details), --network (all interfaces)");
    }
    if (can_use("activity")) {
        sender.sendMessage(kColorGray + "Activity: --page <number>");
    }
}

}  // namespace spark
