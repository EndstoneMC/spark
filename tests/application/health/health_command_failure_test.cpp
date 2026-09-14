#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "../../net/native_exit_gate.h"
#include "application/health/health_command.h"
#include "core/command/arguments.h"

namespace spark {

struct HealthDashboardTestAccess {
    static void setGeneration(HealthDashboard &dashboard, std::uint64_t generation)
    {
        dashboard.generation_ = generation;
    }

    static void markFailure(HealthDashboard &dashboard) { dashboard.failed_.store(true); }
};

struct HealthCommandTestAccess {
    static void prepareUpload(HealthCommand &command)
    {
        command.upload_result_ = UploadResult{.ok = true, .key = "health-key"};
        command.upload_sender_ = "Alice";
        command.upload_sender_is_player_ = true;
        command.uploading_.store(true);
    }

    static void announceUpload(HealthCommand &command) { command.announceHealthUpload(); }

    static void prepareStaleCompletion(HealthCommand &command)
    {
        command.accepted_dashboard_generation_ = 2;
        command.dashboard_sender_ = "NewSender";
        HealthDashboardTestAccess::setGeneration(*command.dashboard_, 2);
    }

    static void prepareCurrentFailure(HealthCommand &command)
    {
        command.accepted_dashboard_generation_ = 2;
        command.dashboard_sender_ = "CurrentSender";
        HealthDashboardTestAccess::setGeneration(*command.dashboard_, 2);
    }

    static void tickAt(HealthCommand &command, std::int64_t now_ms) { command.onTickAt(now_ms); }

    static void markDashboardFailure(HealthCommand &command)
    {
        HealthDashboardTestAccess::markFailure(*command.dashboard_);
    }

    static void complete(HealthCommand &command, HealthDashboard::OpenResult result)
    {
        command.completeHealthDashboard(std::move(result));
    }

    static bool uploading(const HealthCommand &command) { return command.uploading_.load(); }
    static const std::string &dashboardSender(const HealthCommand &command) { return command.dashboard_sender_; }
};

}  // namespace spark

namespace {

class Metadata final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ServerMetadata &, std::int64_t) override {}
    void gatherWorldMetadata(spark::WorldInfo &, std::string_view) override {}
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }
};

class Dispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override
    {
        if (delayed) {
            pending.push_back(std::move(task));
        }
        else {
            task();
        }
    }
    void drain()
    {
        auto tasks = std::move(pending);
        pending.clear();
        for (auto &task : tasks) {
            task();
        }
    }
    bool delayed = false;
    std::vector<std::function<void()>> pending;
};

class ThrowingNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string &sender, const std::string &message) override
    {
        messages.emplace_back(sender, message);
        throw std::runtime_error("notify failed");
    }
    std::vector<std::pair<std::string, std::string>> messages;
};

class Sender final : public spark::CommandSender {
public:
    [[nodiscard]] std::string getName() const override { return "Tester"; }
    [[nodiscard]] bool isPlayer() const override { return false; }

private:
    void sendImpl(const std::string &) override {}
    void errorImpl(const std::string &) override {}
};

void test_upload_state_clears_before_throwing_notification()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-health-failure-viewers.json");
    spark::HealthCommand command(statistics, metadata, {}, "https://viewer/", {}, trusted, dispatcher, notifier);

    spark::HealthCommandTestAccess::prepareUpload(command);
    spark::HealthCommandTestAccess::announceUpload(command);
    assert(!spark::HealthCommandTestAccess::uploading(command));
}

void test_stale_dashboard_completion_is_ignored()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-health-stale-viewers.json");
    spark::HealthCommand command(statistics, metadata, {}, {}, {}, trusted, dispatcher, notifier);

    spark::HealthCommandTestAccess::prepareStaleCompletion(command);
    spark::HealthDashboard::OpenResult stale;
    stale.generation = 1;
    stale.sender_name = "OldSender";
    stale.ok = true;
    stale.url = "https://old/";
    spark::HealthCommandTestAccess::complete(command, std::move(stale));
    assert(spark::HealthCommandTestAccess::dashboardSender(command) == "NewSender");
}

void test_current_failed_completion_clears_state_when_notification_throws()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-health-current-failure.json");
    spark::HealthCommand command(statistics, metadata, {}, {}, {}, trusted, dispatcher, notifier);

    spark::HealthCommandTestAccess::prepareCurrentFailure(command);
    spark::HealthDashboard::OpenResult current;
    current.generation = 2;
    current.sender_name = "CurrentSender";
    current.ok = false;
    spark::HealthCommandTestAccess::complete(command, std::move(current));
    assert(spark::HealthCommandTestAccess::dashboardSender(command).empty());
}

void test_tick_failure_clears_state_when_notification_throws()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-health-tick-failure.json");
    spark::HealthCommand command(statistics, metadata, {}, {}, {}, trusted, dispatcher, notifier);

    spark::HealthCommandTestAccess::prepareCurrentFailure(command);
    spark::HealthCommandTestAccess::markDashboardFailure(command);
    spark::HealthCommandTestAccess::tickAt(command, 0);
    assert(spark::HealthCommandTestAccess::dashboardSender(command).empty());
}

void test_upload_shutdown_delivers_cancellation()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    Sender sender;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-health-upload-cancel.json");
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    std::atomic<bool> cancelled{false};

    spark::HealthCommand::UploadFunction upload = [&](const std::string &, const std::string &, const std::string &,
                                                      const std::string &,
                                                      const spark::CancellationToken &cancellation) {
        {
            std::scoped_lock lock(mutex);
            entered = true;
            cv.notify_all();
        }
        if (cancellation.waitForStop(std::chrono::seconds(2))) {
            cancelled.store(true, std::memory_order_release);
        }
        return spark::UploadResult{.error = "cancelled"};
    };
    spark::HealthCommand command(statistics, metadata, {}, "https://viewer/", {}, trusted, dispatcher, notifier, {},
                                 std::move(upload));
    spark::Arguments args({"upload"}, true);
    command.cmdHealth(sender, args);
    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    assert(command.shutdownWithin(std::chrono::seconds(2)));
    assert(cancelled.load(std::memory_order_acquire));
}

void test_upload_timeout_retains_worker_for_later_reap()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    Sender sender;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-health-upload-reap.json");
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;

    spark::HealthCommand::UploadFunction upload = [&](const std::string &, const std::string &, const std::string &,
                                                      const std::string &, const spark::CancellationToken &) {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
        return spark::UploadResult{.error = "released"};
    };
    spark::HealthCommand command(statistics, metadata, {}, "https://viewer/", {}, trusted, dispatcher, notifier, {},
                                 std::move(upload));
    spark::Arguments args({"upload"}, true);
    command.cmdHealth(sender, args);
    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    assert(!command.shutdownWithin(std::chrono::milliseconds::zero()));
    {
        std::scoped_lock lock(mutex);
        release = true;
        cv.notify_all();
    }
    assert(command.shutdownWithin(std::chrono::seconds(2)));
}

void testDelayedCommittedResultSurvivesRetryAndExpiredOwner()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    dispatcher.delayed = true;
    ThrowingNotifier notifier;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-health-delayed.json");
    auto command =
        std::make_unique<spark::HealthCommand>(statistics, metadata, "", "", "", trusted, dispatcher, notifier);
    spark::HealthDashboard::OpenResult result;
    result.accepted = true;
    result.completed = true;
    result.generation = 1;
    result.sender_name = "First";
    spark::HealthCommandTestAccess::complete(*command, result);
    spark::HealthCommandTestAccess::prepareStaleCompletion(*command);
    assert(dispatcher.pending.size() == 1);
    dispatcher.drain();
    assert(notifier.messages.size() == 1);
    assert(notifier.messages[0].first == "First");
    assert(spark::HealthCommandTestAccess::dashboardSender(*command) == "NewSender");
    result.generation = 2;
    result.sender_name = "Second";
    spark::HealthCommandTestAccess::complete(*command, result);
    assert(dispatcher.pending.size() == 1);
    assert(command->shutdownWithin(std::chrono::seconds(2)));
    command.reset();
    dispatcher.drain();
    assert(notifier.messages.size() == 1);
}

void testUploadNativeExitRejectsReplacement()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    Sender sender;
    spark::test::NativeExitGate gate;
    std::atomic<int> calls{0};
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-health-native-exit.json");
    spark::HealthCommand::UploadFunction upload = [&](const auto &, const auto &, const auto &, const auto &,
                                                      const auto &) {
        ++calls;
        spark::test::holdNativeThreadExit(gate);
        return spark::UploadResult{.error = "native exit gate"};
    };
    spark::HealthCommand command(statistics, metadata, {}, {}, {}, trusted, dispatcher, notifier, {},
                                 std::move(upload));
    command.cmdHealth(sender, spark::Arguments({"upload"}, true));
    gate.waitEntered();
    assert(!spark::HealthCommandTestAccess::uploading(command));
    command.cmdHealth(sender, spark::Arguments({"upload"}, true));
    assert(calls.load() == 1);
    const auto begin = std::chrono::steady_clock::now();
    assert(!command.shutdownWithin(std::chrono::milliseconds(20)));
    assert(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(250));
    gate.unblock();
    assert(command.shutdownWithin(std::chrono::seconds(2)));
}

}  // namespace

int main()
{
    testUploadNativeExitRejectsReplacement();
    testDelayedCommittedResultSurvivesRetryAndExpiredOwner();
    test_upload_state_clears_before_throwing_notification();
    test_stale_dashboard_completion_is_ignored();
    test_current_failed_completion_clears_state_when_notification_throws();
    test_tick_failure_clears_state_when_notification_throws();
    test_upload_shutdown_delivers_cancellation();
    test_upload_timeout_retains_worker_for_later_reap();
    return 0;
}
