#include "application/health/health_command.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "application/health/health_report.h"
#include "core/command/arguments.h"
#include "core/stats/ping_statistics.h"
#include "core/util/base64.h"
#include "core/util/format.h"
#include "core/util/monotonic_time.h"
#include "core/ws/crypto.h"
#include "net/bytebin.h"
#include "spark_constants.h"

namespace spark {

HealthCommand::HealthCommand(StatisticsService &statistics, ProfileMetadataProvider &metadata_provider,
                             std::string bytebin_url, std::string viewer_url, std::string bytesocks_host,
                             TrustedViewersState &trusted_viewers, MainThreadDispatcher &dispatcher,
                             ResultNotifier &notifier, HealthDashboard::ConnectionFactory dashboard_factory,
                             UploadFunction upload_function)
    : statistics_(statistics), metadata_provider_(metadata_provider), trusted_viewers_(trusted_viewers),
      dispatcher_(dispatcher), notifier_(notifier), bytebin_url_(std::move(bytebin_url)),
      viewer_url_(std::move(viewer_url)), bytesocks_host_(std::move(bytesocks_host))
{
    upload_fn_ = upload_function ? std::move(upload_function) : UploadFunction(uploadToBytebin);
    if (!dashboard_factory) {
        const std::string bytesocks_host_copy = bytesocks_host_;
        const std::string bytebin_url_copy = bytebin_url_;
        const std::string viewer_url_copy = viewer_url_;
        dashboard_factory = [bytesocks_host_copy, bytebin_url_copy,
                             viewer_url_copy]() -> std::unique_ptr<HealthDashboardConnection> {
            Crypto::KeyPair key_pair = Crypto::generateKeyPair();
            if (key_pair.public_key_x509.empty() || key_pair.private_key_pkcs8.empty()) {
                return {};
            }
            ViewerSocket::Config config;
            config.bytesocks_host = bytesocks_host_copy;
            config.bytebin_url = bytebin_url_copy;
            config.viewer_url = viewer_url_copy;
            config.user_agent = std::string("endstone-spark/") + kVersion;
            config.sampler_interval = 0;
            config.statistics_interval = 10;
            return makeHealthDashboardViewerSocketConnection(std::move(config), std::move(key_pair));
        };
    }
    dashboard_ = std::make_unique<HealthDashboard>(
        std::move(dashboard_factory),
        [this](const HealthData &data, CancellationToken cancellation) {
            return uploadHealthData(data, std::move(cancellation));
        },
        [this](const std::vector<std::uint8_t> &key) {
            return trusted_viewers_.contains(base64Encode(key.data(), key.size()));
        },
        [this](HealthDashboard::OpenResult result) { completeHealthDashboard(std::move(result)); });
    // Lazily create PingStatistics if the platform provides a PlayerPingProvider.
    if (auto *ping_provider = metadata_provider_.playerPingProvider()) {
        ping_statistics_ = std::make_unique<PingStatistics>(*ping_provider);
    }
}

HealthCommand::~HealthCommand()
{
    if (!shutdownWithin(kDefaultShutdownBudget)) {
        std::terminate();
    }
}

bool HealthCommand::shutdownWithin(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
    stopping_.store(true, std::memory_order_release);
    std::atomic_store_explicit(&lifetime_, std::shared_ptr<int>{}, std::memory_order_release);
    upload_cancellation_.requestStop();
    if (dashboard_) {
        dashboard_->requestStop();
    }

    bool dashboard_stopped = true;
    if (dashboard_) {
        dashboard_stopped = dashboard_->shutdownUntil(deadline);
    }

    const bool upload_stopped = upload_thread_.reapUntil(deadline);
    if (upload_stopped) {
        uploading_.store(false, std::memory_order_release);
    }
    return dashboard_stopped && upload_stopped;
}

void HealthCommand::shutdown()
{
    static_cast<void>(shutdownWithin(kDefaultShutdownBudget));
}

void HealthCommand::onTick()
{
    onTickAt(monotonicUnixMillis());
}

void HealthCommand::onTickAt(std::int64_t now_ms)
{
    if (stopping_.load(std::memory_order_acquire) || !dashboard_) {
        return;
    }
    dashboard_->tick();
    if (dashboard_->updateDue(now_ms)) {
        try {
            HealthData snapshot = captureHealthDataForSender(dashboard_sender_, dashboard_sender_is_player_, now_ms,
                                                             dashboard_sender_unique_id_);
            dashboard_->enqueueUpdate(std::move(snapshot), now_ms);
        }
        catch (...) {  // NOLINT(bugprone-empty-catch): dashboard updates are best effort.
        }
    }
    if (dashboard_->consumeFailure()) {
        const std::string sender = dashboard_sender_;
        dashboard_sender_.clear();
        dashboard_sender_unique_id_.clear();
        accepted_dashboard_generation_ = 0;
        if (!sender.empty()) {
            notifyBestEffort(sender, "Health dashboard connection failed.");
        }
    }
}

void HealthCommand::notifyBestEffort(const std::string &sender_name, const std::string &message) noexcept
{
    try {
        notifier_.notify(sender_name, message);
    }
    catch (...) {  // NOLINT(bugprone-empty-catch): tick notifications are best effort.
    }
}

void HealthCommand::pollPing()
{
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }
    if (ping_statistics_ && ping_statistics_->poll()) {
        const PingSummary &summary = ping_statistics_->lastPollSummary();
        statistics_.recordPlayerPing({.mean = summary.mean(),
                                      .max = static_cast<double>(summary.max()),
                                      .min = static_cast<double>(summary.min()),
                                      .median = static_cast<double>(summary.median()),
                                      .percentile95 = static_cast<double>(summary.percentile95th())});
    }
}

std::vector<int> HealthCommand::pingSamples() const
{
    if (!ping_statistics_) {
        return {};
    }
    return ping_statistics_->rollingAverage().rawSamples();
}

void HealthCommand::pollNetwork()
{
    if (!stopping_.load(std::memory_order_acquire)) {
        network_monitor_.poll();
    }
}

std::map<std::string, NetworkInterfaceSnapshot> HealthCommand::networkSnapshots() const
{
    return network_monitor_.snapshot();
}

void HealthCommand::cmdTps(CommandSender &sender)
{
    sendPerformanceReport(sender, statistics_.snapshot());
}

void HealthCommand::cmdPing(CommandSender &sender, const Arguments &args)
{
    if (!ping_statistics_) {
        sender.sendMessage("{}Ping data is not available on this platform.{}", kColorGold, kColorGray);
        return;
    }

    // Query specific player
    auto players = args.stringFlag("player");
    if (!players.empty()) {
        for (const std::string &player_name : players) {
            PlayerPing ping = ping_statistics_->query(player_name);
            if (!ping.found()) {
                sender.sendMessage("{}Ping data is not available for '{}'.{}", kColorGold, player_name, kColorReset);
            }
            else {
                sender.sendMessage("{}Player {}{} {}has {}{} ms ping.{}", kColorGold, kColorReset, ping.name,
                                   kColorGray, kColorGreen, ping.ping, kColorReset);
            }
        }
        return;
    }

    PingSummary summary = ping_statistics_->currentSummary();
    const PingRollingAverage &average = ping_statistics_->rollingAverage();

    if (summary.total() == 0 && average.samples() == 0) {
        sender.sendMessage("{}There is not enough data to show ping averages yet. Please try again later.{}",
                           kColorGold, kColorGray);
        return;
    }

    sender.sendMessage("{}Average Pings {}(min/med/95%ile/max ms){} from now, last 15m:", kColorGold, kColorGray,
                       kColorReset);
    sender.sendMessage("  {} ;  {}", formatPingRtts(summary), formatPingRtts(average));
}

void HealthCommand::showHealth(CommandSender &sender, const Arguments &args)
{
    showHealthReport(sender, statistics_, metadata_provider_, network_monitor_.snapshot(), args.boolFlag("memory"),
                     args.boolFlag("network"));
}

void HealthCommand::cmdHealth(CommandSender &sender, const Arguments &args)
{
    if (stopping_.load(std::memory_order_acquire)) {
        sender.sendMessage("Health services are shutting down.");
        return;
    }
    std::string action = args.subCommand();
    if (action == "health") {
        action.clear();
    }
    if (action == "show") {
        showHealth(sender, args);
    }
    else if (action == "trust-viewer") {
        trustViewer(sender, args);
    }
    else if (action == "upload" || args.boolFlag("upload")) {
        uploadHealthReport(sender);
    }
    else {
        openHealthDashboard(sender);
    }
}

}  // namespace spark
