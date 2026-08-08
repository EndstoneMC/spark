#ifndef SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H
#define SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profile_exporter.h"
#include "core/command/arguments.h"
#include "core/profiler/profiler.h"
#include "core/stats/statistics_service.h"

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

    // Called every server tick.
    void onTick(double mspt);

    // Sets the server main thread ID (identified lazily).
    void setMainThreadId(std::uint64_t tid) { main_tid_ = tid; }

    // Lifecycle.
    void shutdown();
    bool shutdownBackend(std::string &error) { return profiler_.shutdown(error); }
    bool running() const { return profiler_.running(); }
    bool exporting() const { return exporting_.load(); }

private:
    void sendAllocationHookCoverage(CommandSender &sender);
    void finishProfiler(const std::string &sender_name, bool save,
                        const std::string &comment);
    void runExport();
    void announceResult();

    StatisticsService &statistics_;
    std::string bds_executable_sha256_;
    std::uint64_t main_tid_ = 0;

    MainThreadDispatcher &dispatcher_;
    ProfileMetadataProvider &metadata_provider_;
    ResultNotifier &notifier_;

    Profiler profiler_;
    ProfileExporter exporter_;

    std::atomic<bool> exporting_{false};
    std::string start_sender_name_ = "CONSOLE";
    std::thread export_thread_;

    // Export params, set on the main thread before runExport() runs on export_thread_.
    ExportContext pending_ctx_;
    bool pending_save_ = false;
    std::string pending_sender_ = "CONSOLE";
    std::string pending_result_;
    ExportOutcome pending_outcome_ = ExportOutcome::Failed;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_PROFILER_SERVICE_H
