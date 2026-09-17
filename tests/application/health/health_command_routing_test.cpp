#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "application/health/health_command.h"
#include "core/config/trusted_viewers.h"
#include "core/util/base64.h"
#include "core/util/format.h"

namespace spark {

struct HealthCommandTestAccess {
    static void onTickAt(HealthCommand &command, std::int64_t now_ms) { command.onTickAt(now_ms); }
    static std::int64_t dashboardOpenTimeMs(const HealthCommand &command) { return command.dashboard_open_time_ms_; }
    static bool dashboardUpdateDue(const HealthCommand &command, std::int64_t now_ms)
    {
        return command.dashboard_ != nullptr && command.dashboard_->updateDue(now_ms);
    }
    static bool reapUploadUntil(HealthCommand &command, std::chrono::steady_clock::time_point deadline)
    {
        return command.upload_thread_.reapUntil(deadline);
    }
};

namespace {

class Sender final : public CommandSender {
public:
    [[nodiscard]] std::string getName() const override { return "Alice"; }
    [[nodiscard]] bool isPlayer() const override { return true; }

    std::vector<std::string> messages;
    std::vector<std::string> errors;

private:
    void sendImpl(const std::string &message) override { messages.push_back(message); }
    void errorImpl(const std::string &message) override { errors.push_back(message); }
};

class Dispatcher final : public MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override { task(); }
};

class PingProvider final : public PlayerPingProvider {
public:
    std::map<std::string, int> poll() override { return players; }

    std::map<std::string, int> players;
};

class Notifier final : public ResultNotifier {
public:
    void notify(const std::string &, const std::string &message) override
    {
        std::scoped_lock lock(mutex);
        messages.push_back(message);
        cv.notify_all();
    }

    bool waitForMessageCount(std::size_t count)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(2), [&] { return messages.size() >= count; });
    }

    bool waitForMessageCountUntil(std::size_t count, std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock lock(mutex);
        return cv.wait_until(lock, deadline, [&] { return messages.size() >= count; });
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> messages;
};

class Metadata final : public ProfileMetadataProvider {
public:
    void gatherServerMetadata(ServerMetadata &metadata, std::int64_t) override
    {
        metadata.endstone_version = "test-endstone";
        metadata.minecraft_version = "test-minecraft";
        metadata.player_count = 3;
        metadata.uptime_ms = 12000;
        metadata.server_configurations["server.properties"] = "{}";
    }
    void gatherWorldMetadata(WorldInfo &, std::string_view) override {}
    std::int64_t serverUptimeSeconds() override { return 12; }
    std::int64_t playerCount() override { return 3; }
    PlayerPingProvider *playerPingProvider() override { return ping_provider; }

    PlayerPingProvider *ping_provider = nullptr;
};

struct ConnectionProbe {
    std::mutex mutex;
    std::condition_variable cv;
    int open_count = 0;
    int upload_count = 0;
    int send_count = 0;
    bool open = false;
    bool client = false;
    std::optional<SocketChannelInfo> channel;
};

class Connection final : public HealthDashboardConnection {
public:
    explicit Connection(ConnectionProbe &probe) : probe_(probe) {}

    std::string open(const UploadCallback &upload, const CancellationToken &cancellation) override
    {
        if (cancellation.stopRequested()) {
            return {};
        }
        const UploadResult result = upload(cancellation);
        std::scoped_lock lock(probe_.mutex);
        ++probe_.open_count;
        ++probe_.upload_count;
        probe_.open = result.ok;
        probe_.cv.notify_all();
        return result.ok ? "https://viewer/" + result.key : std::string();
    }
    bool tick() override
    {
        std::scoped_lock lock(probe_.mutex);
        return probe_.open;
    }
    [[nodiscard]] bool isOpen() const override
    {
        std::scoped_lock lock(probe_.mutex);
        return probe_.open;
    }
    [[nodiscard]] bool hasClient() const override
    {
        std::scoped_lock lock(probe_.mutex);
        return probe_.client;
    }
    void requestStop() noexcept override
    {
        std::scoped_lock lock(probe_.mutex);
        probe_.open = false;
        probe_.cv.notify_all();
    }
    bool closeWithin(std::chrono::milliseconds) noexcept override
    {
        close();
        return true;
    }
    void close() override
    {
        std::scoped_lock lock(probe_.mutex);
        probe_.open = false;
        probe_.cv.notify_all();
    }
    [[nodiscard]] SocketChannelInfo channelInfo() const override
    {
        return {.channel_id = "health-channel", .public_key = {1, 2, 3}};
    }
    bool sendStatistics(const std::string &, const std::string &, const std::string &) override
    {
        std::scoped_lock lock(probe_.mutex);
        ++probe_.send_count;
        probe_.cv.notify_all();
        return probe_.open && probe_.client;
    }
    [[nodiscard]] std::vector<std::uint8_t> pendingKey(const std::string &id) const override
    {
        return id == "pending" ? std::vector<std::uint8_t>{9, 8, 7} : std::vector<std::uint8_t>{};
    }
    void sendClientTrusted(const std::string &id) override { trusted_id_ = id; }
    void setIsKeyTrustedCallback(IsKeyTrustedCallback callback) override { trusted_ = std::move(callback); }

private:
    ConnectionProbe &probe_;
    std::string trusted_id_;
    IsKeyTrustedCallback trusted_;
};

struct Fixture {
    Fixture()
        : trusted_viewers(std::filesystem::temp_directory_path() / "spark-health-command-routing-test.json"),
          activity_log(std::filesystem::temp_directory_path() / "spark-health-command-routing-test.json.log")
    {
        std::filesystem::remove(trustedViewersFile());
        std::filesystem::remove(activityLogFile());
    }

    ~Fixture()
    {
        std::filesystem::remove(trustedViewersFile());
        std::filesystem::remove(activityLogFile());
    }

    static std::filesystem::path trustedViewersFile()
    {
        return std::filesystem::temp_directory_path() / "spark-health-command-routing-test.json";
    }
    static std::filesystem::path activityLogFile()
    {
        return std::filesystem::temp_directory_path() / "spark-health-command-routing-test.json.log";
    }

    StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    Notifier notifier;
    TrustedViewersState trusted_viewers;
    ActivityLog activity_log;
    ConnectionProbe connection_probe;
    int factory_count = 0;
};

HealthCommand makeCommand(Fixture &fixture, HealthDashboard::ConnectionFactory factory = {},
                          HealthCommand::UploadFunction upload = {})
{
    return {fixture.statistics,      fixture.metadata,   "https://bytebin/", "https://viewer/",  "bytesocks",
            fixture.trusted_viewers, fixture.dispatcher, fixture.notifier,   std::move(factory), std::move(upload)};
}

std::string stripColors(const std::string &message)
{
    std::string stripped;
    stripped.reserve(message.size());
    for (std::size_t i = 0; i < message.size();) {
        if (i + 2 < message.size() && static_cast<unsigned char>(message[i]) == 0xc2 &&
            static_cast<unsigned char>(message[i + 1]) == 0xa7) {
            i += 3;
        }
        else {
            stripped.push_back(message[i++]);
        }
    }
    return stripped;
}

void testMissingPingMessage()
{
    Fixture fixture;
    PingProvider ping_provider;
    fixture.metadata.ping_provider = &ping_provider;
    auto health = makeCommand(fixture);
    Sender sender;
    health.cmdPing(sender, Arguments({"--player", "MissingSparkProbe"}, false));

    const std::string expected = "Ping data is not available for 'MissingSparkProbe'.";
    assert(sender.messages.size() == 1);
    assert(stripColors(sender.messages.front()) == expected);
    assert(sender.messages.front().starts_with(kColorGold));
    assert(sender.messages.front().ends_with(kColorReset));
    assert(sender.messages.front() == kColorGold + expected + kColorReset);
}

}  // namespace
}  // namespace spark

int main()
{
    using spark::Arguments;
    using spark::base64Encode;
    using spark::CancellationToken;
    using spark::Connection;
    using spark::Fixture;
    using spark::HealthCommandTestAccess;
    using spark::HealthDashboard;
    using spark::HealthDashboardConnection;
    using spark::Sender;
    using spark::UploadResult;
    spark::testMissingPingMessage();
    Fixture fixture;

    int uploads = 0;
    auto upload = [&uploads](const std::string &, const std::string &, const std::string &, const std::string &,
                             const CancellationToken &) {
        ++uploads;
        return UploadResult{.ok = true, .key = "health-key"};
    };
    {
        auto health = makeCommand(fixture, {}, upload);
        Sender sender;
        health.cmdHealth(sender, Arguments({"show"}, true));
        assert(uploads == 0);
        assert(sender.errors.empty());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        health.cmdHealth(sender, Arguments({"upload"}, true));
        assert(fixture.notifier.waitForMessageCountUntil(1, deadline));
        assert(HealthCommandTestAccess::reapUploadUntil(health, deadline));
        assert(uploads == 1);
        health.cmdHealth(sender, Arguments({"health", "--upload"}, true));
        assert(fixture.notifier.waitForMessageCount(2));
    }

    auto factory = [&fixture]() {
        ++fixture.factory_count;
        return std::unique_ptr<HealthDashboardConnection>(std::make_unique<Connection>(fixture.connection_probe));
    };
    {
        auto health = makeCommand(fixture, HealthDashboard::ConnectionFactory(std::move(factory)), upload);
        health.setActivityLogProvider([&fixture]() { return &fixture.activity_log; });
        Sender sender;
        health.cmdHealth(sender, Arguments({}, true));
        health.cmdHealth(sender, Arguments({"unknown"}, true));
        assert(sender.messages.back().find("already open") != std::string::npos);
        assert(fixture.notifier.waitForMessageCount(3));
        assert(fixture.notifier.messages.back().find("https://viewer/initial") != std::string::npos ||
               fixture.notifier.messages.back().find("health-key") != std::string::npos);

        const std::int64_t initial_time_ms = HealthCommandTestAccess::dashboardOpenTimeMs(health);
        HealthCommandTestAccess::onTickAt(health, initial_time_ms + 9999);
        HealthCommandTestAccess::onTickAt(health, initial_time_ms + 10000);
        {
            std::scoped_lock lock(fixture.connection_probe.mutex);
            assert(fixture.connection_probe.send_count == 0);
            fixture.connection_probe.client = true;
        }
        assert(!HealthCommandTestAccess::dashboardUpdateDue(health, initial_time_ms + 9999));
        assert(HealthCommandTestAccess::dashboardUpdateDue(health, initial_time_ms + 10000));
        HealthCommandTestAccess::onTickAt(health, initial_time_ms + 10000);
        HealthCommandTestAccess::onTickAt(health, initial_time_ms + 10000);
        HealthCommandTestAccess::onTickAt(health, initial_time_ms + 10001);
        {
            std::unique_lock lock(fixture.connection_probe.mutex);
            assert(fixture.connection_probe.cv.wait_for(lock, std::chrono::seconds(5),
                                                        [&] { return fixture.connection_probe.send_count == 1; }));
        }
        health.cmdHealth(sender, Arguments({"trust-viewer", "--id", "pending"}, true));
        const std::vector<std::uint8_t> trusted_key{9, 8, 7};
        assert(fixture.trusted_viewers.contains(base64Encode(trusted_key.data(), trusted_key.size())));
        assert(sender.messages.back().find("now trusted") != std::string::npos);
        assert(fixture.activity_log.entries().size() == 1);
        health.shutdown();
        {
            std::scoped_lock lock(fixture.connection_probe.mutex);
            assert(fixture.connection_probe.send_count == 1);
        }
    }
    assert(fixture.factory_count == 1);
    return 0;
}
