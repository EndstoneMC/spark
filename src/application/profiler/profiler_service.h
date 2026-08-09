#ifndef SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H
#define SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profile_exporter.h"
#include "core/activity/activity_log.h"
#include "core/command/arguments.h"
#include "core/config/trusted_viewers.h"
#include "core/profiler/profiler.h"
#include "core/stats/network_monitor.h"
#include "core/stats/statistics_service.h"
#include "core/ws/viewer_socket.h"

namespace spark {

struct ProfilerServiceTestAccess;

// Manages the profiling session lifecycle and background export. No Endstone dependency.
class ProfilerService {
public:
    ProfilerService(StatisticsService &statistics, std::string bds_executable_sha256,
                    std::filesystem::path profile_storage_dir, std::string bytebin_url, std::string viewer_url,
                    std::string bytesocks_host, bool background_enabled, int background_interval,
                    std::string background_thread_grouper, std::string background_thread_dumper,
                    TrustedViewersState &trusted_viewers, MainThreadDispatcher &dispatcher,
                    ProfileMetadataProvider &metadata_provider, ResultNotifier &notifier);
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
    void setNetworkSnapshotProvider(std::function<std::map<std::string, NetworkInterfaceSnapshot>()> provider)
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

    // Heartbeats from the execution sampler's service threads.
    const Heartbeat &samplerHeartbeat() const { return profiler_.samplerHeartbeat(); }
    const Heartbeat &aggregatorHeartbeat() const { return profiler_.aggregatorHeartbeat(); }

    // Sets the directory for crash-safe recovery journals.
    void setRecoveryDirectory(std::filesystem::path dir) { profiler_.setRecoveryDirectory(std::move(dir)); }

    void journalStallBegin(std::uint64_t detected_ns, std::uint64_t last_tick_ns)
    {
        profiler_.journalStallBegin(detected_ns, last_tick_ns);
    }
    void journalStallEnd(std::uint64_t detected_ns, std::uint64_t recovered_ns)
    {
        profiler_.journalStallEnd(detected_ns, recovered_ns);
    }

    // Starts the background profiler if configured. Called on enable.
    void startBackgroundProfiler();

private:
    friend struct ProfilerServiceTestAccess;

    enum class SessionType {
        None,
        Background,
        Foreground
    };
    bool background_started_ = false;
    bool background_suppressed_ = false;
    void sendAllocationHookCoverage(CommandSender &sender);
    void finishProfiler(const std::string &sender_name, bool sender_is_player, bool save, const std::string &comment);
    void runExport() noexcept;
    void announceResult();
    bool startBackgroundSession();
    void closeViewerSocket();
    bool startViewerWorker();
    void stopViewerWorker();
    void viewerUpdateLoop() noexcept;
    void completeViewerOpen(std::uint64_t generation);
    ExportContext captureLiveContext(std::int64_t now_ms);
    std::string buildLiveSamplerData(const ExportContext &context);
    std::string uploadSamplerData(const ExportContext &context);
    bool viewerGenerationCurrent(std::uint64_t generation) const;

    StatisticsService &statistics_;
    std::string bds_executable_sha256_;
    std::uint64_t main_tid_ = 0;

    MainThreadDispatcher &dispatcher_;
    ProfileMetadataProvider &metadata_provider_;
    ResultNotifier &notifier_;

    Profiler profiler_;
    ProfileExporter exporter_;

    std::atomic<bool> exporting_{false};
    std::atomic<bool> export_completion_pending_{false};
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

    std::shared_ptr<ViewerSocket> viewer_socket_;
    std::int64_t last_viewer_upload_ms_ = 0;

    struct ViewerWorkItem {
        enum class Type {
            Open,
            Update
        };
        Type type = Type::Update;
        ExportContext context;
        std::shared_ptr<ViewerSocket> socket;
        std::uint64_t generation = 0;
        std::string sender_name;
    };

    std::thread viewer_update_thread_;
    mutable std::mutex viewer_update_mutex_;
    std::condition_variable viewer_update_cv_;
    std::atomic<bool> viewer_worker_running_{false};
    std::atomic<bool> viewer_worker_failed_{false};
    std::optional<ViewerWorkItem> viewer_work_;
    bool viewer_work_active_ = false;
    bool viewer_open_pending_ = false;
    std::uint64_t viewer_generation_ = 0;
    std::string pending_viewer_url_;
    std::string pending_viewer_sender_;
    std::shared_ptr<ViewerSocket> completed_viewer_socket_;
    std::function<std::string(ViewerSocket &, const ViewerSocket::UploadCallback &)> viewer_open_fn_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);

    // Background profiler retry backoff.
    std::int64_t next_background_retry_ms_ = 0;
    int background_retry_delay_s_ = 0;

    std::string bytebin_url_;
    std::string viewer_url_;
    std::string bytesocks_host_;
    TrustedViewersState &trusted_viewers_;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H
