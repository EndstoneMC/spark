#include <cassert>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "application/activity/activity_command.h"

namespace {

class TestSender final : public spark::CommandSender {
public:
    std::string getName() const override { return "Test"; }
    bool isPlayer() const override { return false; }

    std::vector<std::string> messages;
    std::vector<std::string> errors;

private:
    void sendImpl(const std::string &message) override { messages.push_back(message); }
    void errorImpl(const std::string &message) override { errors.push_back(message); }
};

void runPage(spark::ActivityCommand &command, const std::string &value, TestSender &sender)
{
    command.cmdActivity(sender, spark::Arguments({"activity", "--page", value}));
}

}  // namespace

int main()
{
    const auto path = std::filesystem::temp_directory_path() / "spark_activity_command_test.json";
    std::filesystem::remove(path);
    spark::ActivityLog log(path);
    for (int i = 0; i < 9; ++i) {
        log.add(spark::Activity::url("Test", false,
                                     std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count(),
                                     "Profiler", "https://example.com/" + std::to_string(i)));
    }

    spark::ActivityCommand command(log);
    TestSender sender;

    runPage(command, "0", sender);
    assert(sender.errors.size() == 1);
    sender = {};
    runPage(command, "-1", sender);
    assert(sender.errors.size() == 1);
    sender = {};
    runPage(command, "1", sender);
    assert(sender.errors.empty() && sender.messages.size() == 17);
    sender = {};
    runPage(command, "3", sender);
    assert(sender.errors.empty() && sender.messages.size() == 5);
    sender = {};
    runPage(command, "4", sender);
    assert(sender.errors.empty() && sender.messages.size() == 1);
    sender = {};
    runPage(command, std::to_string((std::numeric_limits<int>::max)()), sender);
    assert(sender.messages.size() == 1);
    sender = {};
    runPage(command, std::to_string(static_cast<long long>((std::numeric_limits<int>::max)()) + 1), sender);
    assert(sender.messages.size() == 1 || sender.errors.size() == 1);
    sender = {};
    runPage(command, std::to_string((std::numeric_limits<long>::max)()), sender);
    assert(sender.messages.size() == 1);

    std::filesystem::remove(path);
    return 0;
}
