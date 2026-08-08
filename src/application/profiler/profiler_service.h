#ifndef SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H
#define SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profile_exporter.h"
#include "core/activity/activity_log.h"
#include "core/command/arguments.h"
#include "core/config/spark_config.h"
#include "core/profiler/profiler.h"
#include "core/stats/network_monitor.h"
#include "core/stats/statistics_service.h"
#include "core/ws/viewer_socket.h"

namespace spark {

// Owns the Profiler and manages the profiling session lifecycle:
// start, stop, cancel, timeout auto-stop, and background export.
// Platform access is through injected capabilities (dispatcher,
// metadata provider, notifier). No Endstone dependency.
class ProfilerService {
public:
    ProfilerService(StatisticsService &statistics,
                    std::string bds_executable_sha256,
                    std::filesystem::path profile_storage_dir,
                    std::string bytebin_url,
                    std::string viewer_url,
                    bool background_enabled,
                    int background_interval,
                    std::string background_thread_grouper,
                    std::string background_thread_dumper,
                    SparkConfig &config,
                    MainThreadDispatcher &dispatcher,
                    ProfileMetadataProvider &metadata_provider,
                    ResultNotifier &notifier);
    ~ProfilerService();

    ProfilerService(const ProfilerService &) = delete;
    ProfilerService &operator=(const ProfilerService &) = delete;

    // Command handlers (called on the main thread by command dispatch).
    void cmdStart(CommandSender &sender, const Arguments &args);
    void cmdStop(CommandSender &sender, const Arguments &args);
    void cmdInfo(CommandSender &sender);
    void cmdCancel(CommandSender &sender);
    void cmdOpen(CommandSender &sender);
    void cmdTrustViewer(CommandSender &sender, const Arguments &args);

    // Called every server tick.
    void onTick(double mspt);

    // Sets the server main thread ID (identified lazily).
    void setMainThreadId(std::uint64_t tid) { main_tid_ = tid; }

    // Sets a callback that returns the current ping samples for export.
    void setPingSamplesProvider(std::function<std::vector<int>()> provider)
    {
        ping_samples_provider_ = std::move(provider);
    }

    // Sets a callback that returns the current network snapshots for export.
    void setNetworkSnapshotProvider(
        std::function<std::map<std::string, NetworkInterfaceSnapshot>()> provider)
    {
        network_snapshot_provider_ = std::move(provider);
    }

    // Sets a callback that returns the activity log, or nullptr if not available.
    void setActivityLogProvider(std::function<ActivityLog *()> provider)
    {
        activity_log_provider_ = std::move(provider);
    }

    // Lifecycle.
    void shutdown();
    bool shutdownBackend(std::string &error) { return profiler_.shutdown(error); }
    bool running() const { return profiler_.running(); }
    bool exporting() const { return exporting_.load(); }
    bool isBackgroundRunning() const { return session_type_ == SessionType::Background; }

    // Starts the background profiler if configured. Called on enable.
    void startBackgroundProfiler();

private:
    enum class SessionType { None, Background, Foreground };
    bool background_started_ = false;
    void sendAllocationHookCoverage(CommandSender &sender);
    void finishProfiler(const std::string &sender_name, bool sender_is_player,
                        bool save, const std::string &comment);
    void runExport();
    void announceResult();
    bool startBackgroundSession();
    void closeViewerSocket();
    void startViewerWorker();
    void stopViewerWorker();
    void viewerUpdateLoop();
    std::string uploadSamplerData(const std::string &channel_info_proto);

    StatisticsService &statistics_;
    std::string bds_executable_sha256_;
    std::uint64_t main_tid_ = 0;

    MainThreadDispatcher &dispatcher_;
    ProfileMetadataProvider &metadata_provider_;
    ResultNotifier &notifier_;

    Profiler profiler_;
    ProfileExporter exporter_;

    std::atomic<bool> exporting_{false};
    SessionType session_type_ = SessionType::None;
    bool restart_background_after_export_ = false;
    bool background_enabled_ = true;
    int background_interval_ = 10;
    std::string background_thread_grouper_ = "by-pool";
    std::string background_thread_dumper_ = "default";
    std::string start_sender_name_ = "CONSOLE";
    bool start_sender_is_player_ = false;
    std::thread export_thread_;

    // Export params, set on the main thread before runExport() runs on export_thread_.
    ExportContext pending_ctx_;
    bool pending_save_ = false;
    std::string pending_sender_ = "CONSOLE";
    bool pending_sender_is_player_ = false;
    std::string pending_result_;
    ExportOutcome pending_outcome_ = ExportOutcome::Failed;
    std::function<std::vector<int>()> ping_samples_provider_;
    std::function<std::map<std::string, NetworkInterfaceSnapshot>()> network_snapshot_provider_;
    std::function<ActivityLog *()> activity_log_provider_;

    std::unique_ptr<ViewerSocket> viewer_socket_;
    std::int64_t last_viewer_upload_ms_ = 0;

    // Viewer update worker: moves live-export + gzip + HTTP upload off the
    // main thread so 10-second viewer rotations don't stall server ticks.
    std::thread viewer_update_thread_;
    std::mutex viewer_update_mutex_;
    std::condition_variable viewer_update_cv_;
    std::atomic<bool> viewer_worker_running_{false};
    std::atomic<bool> viewer_update_requested_{false};

    // Background profiler retry backoff.
    std::int64_t next_background_retry_ms_ = 0;
    int background_retry_delay_s_ = 0;

    std::string bytebin_url_;
    std::string viewer_url_;
    SparkConfig &config_;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H
