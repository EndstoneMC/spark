#ifndef SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H
#define SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"
#include "core/activity/activity_log.h"
#include "core/command/arguments.h"
#include "core/stats/network_monitor.h"
#include "core/stats/ping_statistics.h"
#include "core/stats/statistics_service.h"
#include "net/bytebin.h"
#include "proto/sampler_data.h"

namespace spark {

struct HealthCommandTestAccess;

// Handles /spark tps, /spark ping, and /spark health commands.
// Platform-independent: uses CommandSender and ProfileMetadataProvider.
class HealthCommand {
public:
    HealthCommand(StatisticsService &statistics, ProfileMetadataProvider &metadata_provider, std::string bytebin_url,
                  std::string viewer_url, MainThreadDispatcher &dispatcher, ResultNotifier &notifier);
    ~HealthCommand();

    void cmdTps(CommandSender &sender);
    void cmdPing(CommandSender &sender, const Arguments &args);
    void cmdHealth(CommandSender &sender, const Arguments &args);

    // Called periodically (every ~10 seconds) to poll ping data.
    void pollPing();

    // Called periodically (every ~60 seconds) to poll network interface stats.
    void pollNetwork();

    // Returns the current ping samples for profile export, or an empty vector
    // if ping monitoring is not active.
    std::vector<int> pingSamples() const;

    // Returns the current network interface snapshots for profile export.
    std::map<std::string, NetworkInterfaceSnapshot> networkSnapshots() const;
    void shutdown();

    // Sets a callback that returns the activity log, or nullptr if not available.
    void setActivityLogProvider(std::function<ActivityLog *()> provider)
    {
        activity_log_provider_ = std::move(provider);
    }

private:
    friend struct HealthCommandTestAccess;

    void sendPerformanceReport(CommandSender &sender, const StatisticsSnapshot &stats);
    void uploadHealthReport(CommandSender &sender);
    HealthData captureHealthData(const CommandSender &sender, std::int64_t now_ms);
    void runHealthUpload(HealthData data, std::string sender_name, bool sender_is_player, std::int64_t now_ms);
    void announceHealthUpload();

    StatisticsService &statistics_;
    ProfileMetadataProvider &metadata_provider_;
    MainThreadDispatcher &dispatcher_;
    ResultNotifier &notifier_;
    std::unique_ptr<PingStatistics> ping_statistics_;
    NetworkMonitor network_monitor_;
    std::function<ActivityLog *()> activity_log_provider_;
    std::string bytebin_url_;
    std::string viewer_url_;
    std::thread upload_thread_;
    std::atomic<bool> uploading_{false};
    std::mutex upload_mutex_;
    UploadResult upload_result_;
    std::string upload_sender_;
    bool upload_sender_is_player_ = false;
    std::int64_t upload_time_ms_ = 0;
    std::function<UploadResult(const std::string &, const std::string &, const std::string &, const std::string &)>
        upload_fn_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
};

}  // namespace spark

#endif  // SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H
