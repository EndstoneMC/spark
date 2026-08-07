#ifndef SPARK_PLATFORM_ENDSTONE_PROFILER_CONTROLLER_H
#define SPARK_PLATFORM_ENDSTONE_PROFILER_CONTROLLER_H

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

#include "core/profiler/profiler.h"
#include "core/stats/statistics_service.h"

namespace endstone {
class CommandSender;
class Plugin;
}  // namespace endstone

namespace spark {
class Arguments;
}  // namespace spark

namespace spark::endstone_adapter {

enum class ExportOutcome {
    Failed,
    Uploaded,
    Saved,
};

// Owns the profiler and export lifecycle.  All profiler command handlers and
// the background export thread live here; the plugin delegates to this object.
class ProfilerController {
public:
    ProfilerController(::endstone::Plugin &plugin,
                       spark::StatisticsService &statistics,
                       const std::string &bds_executable_sha256);
    ~ProfilerController();

    ProfilerController(const ProfilerController &) = delete;
    ProfilerController &operator=(const ProfilerController &) = delete;

    // Dispatches /spark profiler <action>.  main_tid is the server thread id
    // (0 if not yet identified).
    void cmdProfiler(::endstone::CommandSender &sender, const spark::Arguments &args,
                     std::uint64_t main_tid);

    // Called every server tick while the profiler may be running.
    void onTick(double mspt);

    // Joins the export thread if still running.  Call after cancelling
    // scheduler tasks.
    void shutdown();

    // Shuts down the profiler backend (allocation hooks, threads).
    bool shutdownProfiler(std::string &error) { return profiler_.shutdown(error); }

    bool running() const { return profiler_.running(); }
    bool exporting() const { return exporting_.load(); }

private:
    void profilerStart(::endstone::CommandSender &sender, const spark::Arguments &args,
                       std::uint64_t main_tid);
    void profilerStop(::endstone::CommandSender &sender, const spark::Arguments &args);
    void profilerInfo(::endstone::CommandSender &sender);
    void profilerCancel(::endstone::CommandSender &sender);
    void sendAllocationHookCoverage(::endstone::CommandSender &sender);
    void finishProfiler(const std::string &sender_name, bool save,
                        const std::string &comment);
    void runExport();
    void announceResult();
    void announce(const std::string &sender_name, const std::string &text);

    static std::int64_t nowMs();
    static bool commandSenderIsPlayer(const ::endstone::CommandSender &sender);

    ::endstone::Plugin &plugin_;
    spark::StatisticsService &statistics_;
    std::string bds_executable_sha256_;

    spark::Profiler profiler_;
    std::atomic<bool> exporting_{false};
    std::string start_sender_name_ = "CONSOLE";
    std::thread export_thread_;

    // Export params, set on the main thread before runExport() runs on export_thread_.
    spark::ExportContext pending_ctx_;
    std::filesystem::path pending_folder_;
    std::string pending_sender_ = "CONSOLE";
    std::string pending_result_;
    bool pending_save_ = false;
    ExportOutcome pending_outcome_ = ExportOutcome::Failed;
};

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_PROFILER_CONTROLLER_H
