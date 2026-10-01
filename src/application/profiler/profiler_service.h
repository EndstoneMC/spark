#ifndef SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H
#define SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profile_exporter.h"
#include "application/profiler/profiler_open_orchestrator.h"
#include "application/profiler/profiler_timeout.h"
#include "core/activity/activity_log.h"
#include "core/command/arguments.h"
#include "core/config/trusted_viewers.h"
#include "core/profiler/profiler.h"
#include "core/stats/network_monitor.h"
#include "core/stats/statistics_service.h"
#include "core/util/deadline_thread.h"
#include "core/ws/viewer_socket.h"
#include "net/cancellation.h"

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
    void cmdOpen(CommandSender &sender, const Arguments &args);
    void cmdTrustViewer(CommandSender &sender, const Arguments &args);

    // Called every server tick.
    void onTick(double mspt);

    // Sets the server main thread ID (identified lazily).
    void setMainThreadId(std::uint64_t tid)
    {
        main_tid_ = tid;
        if (tid == 0 || !allocation_rate_metrics_enabled_ || allocation_rate_metrics_start_attempted_) {
            return;
        }
        allocation_rate_metrics_start_attempted_ = true;
        std::string error;
        if (!profiler_.setPersistentAllocationCountingEnabled(true, tid, error)) {
            allocation_rate_metrics_enabled_ = false;
        }
    }
    void configureAllocationRateMetrics(bool enabled)
    {
        allocation_rate_metrics_enabled_ = enabled;
        allocation_rate_metrics_start_attempted_ = false;
    }
    [[nodiscard]] bool allocationRateMetricsActive() const { return profiler_.persistentAllocationCountingEnabled(); }
    [[nodiscard]] std::uint64_t persistentAllocationBytes() const { return profiler_.persistentAllocationBytes(); }

    void setPythonStackProvider(PythonStackProvider *provider) noexcept { profiler_.setPythonStackProvider(provider); }
    [[nodiscard]] bool executionProfiling() const
    {
        return profiler_.running() && profiler_.mode() == ProfileMode::Execution;
    }

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
    bool shutdown(std::string &error);
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
    bool reportJournalDegradationIfNeeded(std::string &cause)
    {
        return profiler_.reportJournalDegradationIfNeeded(cause);
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
    void finishProfiler(const std::string &sender_name, bool sender_is_player, std::string sender_unique_id, bool save,
                        const std::string &comment);
    struct ExportJob {
        ExportContext context;
        bool save_to_file = false;
        std::string sender;
        bool sender_is_player = false;
        std::string sender_unique_id;
        CancellationToken cancellation;
        std::shared_ptr<int> lifetime_token;
    };
    struct ExportResult {
        ExportOutcome outcome = ExportOutcome::Failed;
        std::string message;
        std::string sender = "CONSOLE";
        bool sender_is_player = false;
        std::string sender_unique_id;
        bool retain_recovery_journal = false;
    };
    void ensureExportWorker();
    void exportWorkerLoop() noexcept;
    bool waitForExportWorker(std::chrono::milliseconds timeout);
    bool waitForExportWorkerUntil(std::chrono::steady_clock::time_point deadline);
    bool consumeFinishedExport(bool notify) noexcept;
    void announceResult() noexcept;
    void announceResult(ExportResult result) noexcept;
    bool startBackgroundSession() noexcept;
    void closeViewerSocket();
    void resetProfilerTimeout() noexcept;
    void requestProfilerTimeoutStop() noexcept;
    bool resetProfilerTimeoutUntil(std::chrono::steady_clock::time_point deadline) noexcept;
    bool armProfilerTimeout(std::int64_t timeout_seconds) noexcept;
    ExportContext captureLiveContext(std::int64_t now_ms);
    std::string buildLiveSamplerData(const ExportContext &context);
    bool viewerOpenPending() const { return viewer_open_ && viewer_open_->viewerOpenPending(); }

    void setViewerOpenFunctionForTesting(
        std::function<std::string(ViewerSocket &, const ViewerSocket::UploadCallback &)> open_function)
    {
        viewer_open_->setViewerOpenFunctionForTesting(std::move(open_function));
    }
    void setViewerSocketForTesting(std::shared_ptr<ViewerSocket> socket)
    {
        viewer_open_->setViewerSocketForTesting(std::move(socket));
    }
    bool hasViewerSocketForTesting() const { return viewer_open_ && viewer_open_->hasViewerSocket(); }
    std::shared_ptr<ViewerSocket> viewerSocketForTesting() const
    {
        return viewer_open_ ? viewer_open_->viewerSocketForTesting() : nullptr;
    }

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
    mutable std::mutex export_mutex_;
    std::condition_variable export_cv_;
    std::condition_variable export_exit_cv_;
    std::optional<ExportJob> export_job_;
    std::optional<ExportResult> export_result_;
    CancellationSource export_cancellation_;
    bool export_stop_requested_ = false;
    bool export_worker_exited_ = true;
    bool preserve_recovery_journal_on_shutdown_ = false;
    ProfilerTimeout profiler_timeout_;
    std::atomic<std::uint64_t> timeout_completion_pending_{0};
    std::atomic<std::uint64_t> timeout_generation_{1};
    std::atomic<bool> stopping_{false};
    SessionType session_type_ = SessionType::None;
    bool restart_background_after_export_ = false;
    bool background_enabled_ = true;
    int background_interval_ = 10;
    bool allocation_rate_metrics_enabled_ = false;
    bool allocation_rate_metrics_start_attempted_ = false;
    std::string background_thread_grouper_ = "by-pool";
    std::string background_thread_dumper_ = "default";
    std::string start_sender_name_ = "CONSOLE";
    bool start_sender_is_player_ = false;
    std::string start_sender_unique_id_;
    std::vector<NativePluginSource> session_native_plugin_sources_;
    detail::DeadlineThread export_thread_;

    // Legacy test seam; production jobs/results are owned by export_mutex_.
    std::string pending_sender_ = "CONSOLE";
    bool pending_sender_is_player_ = false;
    std::string pending_sender_unique_id_;
    std::string pending_result_;
    ExportOutcome pending_outcome_ = ExportOutcome::Failed;
    std::function<std::vector<int>()> ping_samples_provider_;
    std::function<std::map<std::string, NetworkInterfaceSnapshot>()> network_snapshot_provider_;
    std::function<ActivityLog *()> activity_log_provider_;

    std::unique_ptr<ProfilerOpenOrchestrator> viewer_open_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);

    // Background profiler retry backoff.
    std::int64_t next_background_retry_ms_ = 0;
    int background_retry_delay_s_ = 0;

    std::string bytebin_url_;
    std::string viewer_url_;
    std::string bytesocks_host_;
    TrustedViewersState &trusted_viewers_;

#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
    using ExportFunction =
        std::function<ProfileExporter::Result(Profiler &, const ExportContext &, bool, CancellationToken)>;
    ExportFunction export_function_;
    std::function<void()> export_post_publication_hook_;
    std::function<void()> export_job_preparation_hook_;
    std::chrono::milliseconds export_shutdown_timeout_{5000};
    std::function<void()> timeout_publication_hook_;
    std::thread::id export_worker_id_for_testing_;
#endif
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H
