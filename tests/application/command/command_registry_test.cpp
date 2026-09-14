#include <cassert>
#include <map>
#include <string>
#include <vector>

#include "application/command/command_registry.h"

namespace {

class Sender final : public spark::CommandSender {
public:
    [[nodiscard]] std::string getName() const override { return "Test"; }
    [[nodiscard]] bool isPlayer() const override { return false; }
    [[nodiscard]] bool hasPermission(const std::string &permission) const override
    {
        const auto it = permissions.find(permission);
        return it != permissions.end() && it->second;
    }

    std::map<std::string, bool> permissions;
    std::vector<std::string> messages;
    std::vector<std::string> errors;

private:
    void sendImpl(const std::string &message) override { messages.push_back(message); }
    void errorImpl(const std::string &message) override { errors.push_back(message); }
};

}  // namespace

int main()
{
    spark::CommandRegistry registry;
    std::string subcommand;
    std::vector<std::string> values;
    registry.registerCommand(
        {"profile"}, "profile command", "spark.profile", true,
        [&subcommand](spark::CommandSender &, const spark::Arguments &args) { subcommand = args.subCommand(); });
    registry.registerCommand(
        {"activity"}, "activity command", "spark.activity", false,
        [&values](spark::CommandSender &, const spark::Arguments &args) { values = args.stringFlag("value"); });
    registry.registerCommand({"health"}, "health command", "spark.health", true,
                             [](spark::CommandSender &, const spark::Arguments &) {});
    registry.registerCommand({"public"}, "public command", "", false,
                             [](spark::CommandSender &, const spark::Arguments &) {});

    Sender sender;
    sender.permissions["spark.profile"] = true;
    sender.permissions["spark.activity"] = true;
    registry.dispatch(sender, {"profile", "start"});
    assert(subcommand == "start");
    registry.dispatch(sender, {"activity", "--value", "hello", "world"});
    assert(values == std::vector<std::string>{"hello world"});
    registry.dispatch(sender, {"activity", "unexpected"});
    assert(sender.errors.size() == 1);
    assert(sender.errors.front() == "Expected flag at position 0 but got 'unexpected' instead!");

    sender.messages.clear();
    sender.permissions["spark.profile"] = false;
    registry.sendHelp(sender);
    bool saw_profile = false;
    bool saw_activity = false;
    bool saw_health_details = false;
    bool saw_public = false;
    bool saw_profiler_details = false;
    for (const std::string &message : sender.messages) {
        saw_profile = saw_profile || message.find("/spark profile ") != std::string::npos;
        saw_activity = saw_activity || message.find("/spark activity ") != std::string::npos;
        saw_health_details = saw_health_details || message.find("health show: --memory") != std::string::npos;
        saw_public = saw_public || message.find("/spark public ") != std::string::npos;
        saw_profiler_details = saw_profiler_details || message.find("Modes: --alloc") != std::string::npos;
    }
    assert(!saw_profile);
    assert(saw_activity);
    assert(!saw_health_details);
    assert(saw_public);
    assert(!saw_profiler_details);

    sender.permissions["endstone.command.spark"] = true;
    sender.messages.clear();
    registry.sendHelp(sender);
    assert(sender.messages.size() >= 5);
    saw_health_details = false;
    for (const std::string &message : sender.messages) {
        saw_health_details = saw_health_details || message.find("health show: --memory") != std::string::npos;
    }
    assert(saw_health_details);

    sender.permissions["endstone.command.spark"] = false;
    sender.permissions["spark"] = true;
    sender.messages.clear();
    registry.sendHelp(sender);
    assert(sender.messages.size() >= 5);

    sender.permissions["spark"] = false;
    sender.errors.clear();
    registry.dispatch(sender, {"profile", "start"});
    assert(sender.errors.size() == 1);

    return 0;
}
