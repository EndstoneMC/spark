#include "platform/endstone/profiler_controller.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <endstone/endstone.hpp>

#include "core/command/arguments.h"
#include "core/util/format.h"
#include "net/bytebin.h"
#include "net/gzip.h"
#include "net/profile_file.h"
#include "platform/endstone/server_info.h"
#include "core/profiler/profiler.h"
#include "spark_constants.h"
#include "core/stats/statistics_service.h"
#include "core/stats/system_stats.h"

namespace spark::endstone_adapter {

using endstone::ColorFormat;

namespace {

std::int64_t nowMsImpl()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ProfilerController::ProfilerController(::endstone::Plugin &plugin,
                                       spark::StatisticsService &statistics,
                                       const std::string &bds_executable_sha256)
    : plugin_(plugin),
      statistics_(statistics),
      bds_executable_sha256_(bds_executable_sha256)
{
}

ProfilerController::~ProfilerController()
{
    if (export_thread_.joinable()) {
        export_thread_.join();
    }
}

void ProfilerController::shutdown()
{
    if (export_thread_.joinable()) {
        export_thread_.join();
    }
}

std::int64_t ProfilerController::nowMs()
{
    return nowMsImpl();
}

bool ProfilerController::commandSenderIsPlayer(const ::endstone::CommandSender &sender)
{
    return sender.asPlayer() != nullptr;
}

void ProfilerController::cmdProfiler(::endstone::CommandSender &sender,
                                     const spark::Arguments &args,
                                     std::uint64_t main_tid)
{
    const std::string &action = args.subCommand();
    if (action.empty() || action == "info") {
        profilerInfo(sender);
    }
    else if (action == "cancel") {
        profilerCancel(sender);
    }
    else if (action == "stop") {
        profilerStop(sender, args);
    }
    else if (action == "start") {
        profilerStart(sender, args, main_tid);
    }
    else {
        profilerInfo(sender);
    }
}

void ProfilerController::profilerStart(::endstone::CommandSender &sender,
                                       const spark::Arguments &args,
                                       std::uint64_t main_tid)
{
    if (profiler_.running()) {
        profilerInfo(sender);
        return;
    }
    if (exporting_.load()) {
        sender.sendMessage("The profiler has stopped; results are still being finalized.");
        return;
    }

    spark::ProfilerOptions options;
    options.alloc_live_only = args.boolFlag("alloc-live-only");
    options.alloc = args.boolFlag("alloc") || options.alloc_live_only;
#if !defined(_WIN32) && !defined(__linux__)
    if (options.alloc) {
        sender.sendErrorMessage(
            "The native allocation profiler is supported only on Windows x64 and Linux x86-64.");
        return;
    }
#endif
    options.threads = args.stringFlag("thread");
    options.regex = args.boolFlag("regex");
    if (args.boolFlag("thread") && options.threads.empty()) {
        sender.sendErrorMessage("--thread requires a thread name, pattern, or *.");
        return;
    }
    if (options.regex && options.threads.empty()) {
        sender.sendErrorMessage("--regex requires at least one --thread pattern.");
        return;
    }
    const auto all_selector = std::find(options.threads.begin(), options.threads.end(), "*");
    if (all_selector != options.threads.end() &&
        (options.regex || options.threads.size() != 1)) {
        sender.sendErrorMessage("--thread * cannot be combined with another --thread or --regex.");
        return;
    }

    auto interval = args.doubleFlag("interval");
    if (args.boolFlag("interval") && !interval) {
        sender.sendErrorMessage("The sampling interval must be a finite number.");
        return;
    }
    if (interval && *interval <= 0.0) {
        sender.sendErrorMessage("The sampling interval must be greater than zero.");
        return;
    }

    if (options.alloc) {
        if (interval && *interval > static_cast<double>(spark::kMaxAllocationIntervalBytes)) {
            sender.sendErrorMessage("The allocation interval must not exceed {} bytes.",
                                    spark::kMaxAllocationIntervalBytes);
            return;
        }
        options.allocation_interval_bytes = interval
                                                ? static_cast<std::int32_t>(*interval + 0.5)
                                                : spark::kDefaultAllocationIntervalBytes;
        if (options.allocation_interval_bytes < 1) {
            options.allocation_interval_bytes = 1;
        }
    }
    else {
        if (interval && *interval > spark::kMaxSamplingIntervalMs) {
            sender.sendErrorMessage("The sampling interval must not exceed {}ms.",
                                    spark::kMaxSamplingIntervalMs);
            return;
        }
        options.interval_ms = interval ? static_cast<int>(*interval + 0.5) : 4;
        if (options.interval_ms < 1) {
            options.interval_ms = 1;
        }
    }

    auto timeout_flag = args.intFlag("timeout");
    if (args.boolFlag("timeout") && !timeout_flag) {
        sender.sendErrorMessage("The timeout must be a whole number of seconds.");
        return;
    }
    long timeout = timeout_flag.value_or(-1);
    if (timeout_flag && timeout <= 10) {
        sender.sendErrorMessage("The timeout is too short for useful results - choose a value over 10 seconds.");
        return;
    }
    options.timeout_seconds = timeout;

    auto tick_threshold = args.intFlag("only-ticks-over");
    if (args.boolFlag("only-ticks-over") && !tick_threshold) {
        sender.sendErrorMessage("The tick threshold must be a whole number of milliseconds.");
        return;
    }
    if (tick_threshold && *tick_threshold <= 0) {
        sender.sendErrorMessage("The tick threshold must be greater than 0ms.");
        return;
    }
    options.only_ticks_over_ms = tick_threshold.value_or(-1);
    options.ignore_sleeping = !args.boolFlag("include-sleeping");
    auto comments = args.stringFlag("comment");
    if (!comments.empty()) {
        options.comment = comments.front();
    }
    options.save_to_file = args.boolFlag("save-to-file");
    options.creator_name = sender.getName();
    options.creator_is_player = commandSenderIsPlayer(sender);

    std::uint64_t tid = main_tid;
    if (tid == 0) {
        sender.sendErrorMessage("The server thread hasn't been identified yet - try again in a moment.");
        return;
    }

    std::string error;
    if (!profiler_.start(options, tid, error)) {
        sender.sendErrorMessage("Couldn't start the profiler: {}", error);
        return;
    }
    start_sender_name_ = sender.getName();

    if (options.alloc) {
        if (options.alloc_live_only) {
            sender.sendMessage("{}Retained Allocation Profiler is now running!{} (async)",
                               ColorFormat::Gold, ColorFormat::Gray);
        }
        else {
            sender.sendMessage("{}Allocation Profiler is now running!{} (async)",
                               ColorFormat::Gold, ColorFormat::Gray);
        }
        if (options.threads.empty() ||
            (options.threads.size() == 1 && options.threads.front() == "*")) {
            sender.sendMessage(
                "Sampling approximately every {} of native allocations across process threads.",
                spark::formatBytes(static_cast<std::uint64_t>(
                    options.allocation_interval_bytes)));
        }
        else {
            sender.sendMessage(
                "Sampling approximately every {} of native allocations from matching threads.",
                spark::formatBytes(static_cast<std::uint64_t>(
                    options.allocation_interval_bytes)));
        }
        if (options.alloc_live_only) {
            sender.sendMessage("The result will contain only sampled allocations still live when profiling stops.");
        }
    }
    else {
        if (options.threads.empty()) {
            sender.sendMessage("{}Profiler is now running!{} (async, {}ms interval)", ColorFormat::Gold,
                               ColorFormat::Gray, options.interval_ms);
        }
        else if (options.threads.size() == 1 && options.threads.front() == "*") {
            sender.sendMessage("{}Profiler is now running for all process threads!{} (async, {}ms interval)",
                               ColorFormat::Gold, ColorFormat::Gray, options.interval_ms);
        }
        else {
            sender.sendMessage("{}Profiler is now running for selected process threads!{} (async, {}ms interval)",
                               ColorFormat::Gold, ColorFormat::Gray, options.interval_ms);
        }
    }
    if (options.only_ticks_over_ms > 0) {
        sender.sendMessage("Only recording ticks longer than {}ms.", options.only_ticks_over_ms);
    }
    if (timeout <= 0) {
        sender.sendMessage("It runs in the background until stopped.");
        sender.sendMessage("To stop and finalize the profile, run: {}/spark profiler stop", ColorFormat::Gray);
    }
    else {
        if (timeout < 30) {
            sender.sendMessage("Tip: a timeout over 30s gives noticeably more accurate results.");
        }
        sender.sendMessage("Results will be returned automatically after {}.", spark::formatDuration(timeout));
    }
}

void ProfilerController::profilerStop(::endstone::CommandSender &sender,
                                      const spark::Arguments &args)
{
    if (!profiler_.running()) {
        sender.sendMessage(exporting_.load() ? "The profiler has stopped; results are still being finalized."
                                             : "There isn't an active profiler running.");
        return;
    }
    std::string backend_error;
    if (profiler_.backendFailure(backend_error)) {
        std::string cleanup_error;
        if (!profiler_.cancel(cleanup_error)) {
            sender.sendMessage("{}Allocation profiler status: FAILED", ColorFormat::Red);
            sender.sendMessage("Unable to discard the failed session safely: {}", cleanup_error);
            return;
        }
        sender.sendMessage("{}Allocation profiler status: FAILED", ColorFormat::Red);
        sender.sendMessage("Incomplete profile data was discarded: {}", backend_error);
        sender.sendMessage("The allocation profiler backend is ready for a new session.");
        return;
    }
    bool save = profiler_.options().save_to_file || args.boolFlag("save-to-file");
    std::string comment;
    auto comments = args.stringFlag("comment");
    if (!comments.empty()) {
        comment = comments.front();
    }
    sender.sendMessage("{}Stopping the profiler and finalizing results, please wait...", ColorFormat::Gold);
    finishProfiler(sender.getName(), save, comment);
}

void ProfilerController::profilerInfo(::endstone::CommandSender &sender)
{
    if (!profiler_.running()) {
        if (exporting_.load()) {
            sender.sendMessage("The profiler has stopped; results are still being finalized.");
            return;
        }
        sender.sendMessage("The profiler isn't running!");
        sender.sendMessage("To start a new one, run: {}/spark profiler start", ColorFormat::Gray);
        return;
    }
    const bool allocation = profiler_.mode() == spark::ProfileMode::Allocation;
    std::string backend_error;
    if (allocation && profiler_.backendFailure(backend_error)) {
        sender.sendMessage("{}Allocation Profiler status: FAILED", ColorFormat::Red);
        sender.sendMessage("Backend service failure: {}", backend_error);
        sendAllocationHookCoverage(sender);
        sender.sendMessage("The incomplete profile will not be exported.");
        sender.sendMessage("Run {}/spark profiler stop{} or {}/spark profiler cancel{} to discard it.",
                           ColorFormat::Gray, ColorFormat::Reset, ColorFormat::Gray,
                           ColorFormat::Reset);
        return;
    }
    if (allocation) {
        if (profiler_.options().alloc_live_only) {
            sender.sendMessage("{}Retained Allocation Profiler is already running!", ColorFormat::Gold);
        }
        else {
            sender.sendMessage("{}Allocation Profiler is already running!", ColorFormat::Gold);
        }
        sendAllocationHookCoverage(sender);
        const auto &threads = profiler_.options().threads;
        if (threads.empty() ||
            (threads.size() == 1 && threads.front() == "*")) {
            sender.sendMessage("Thread selection: all process threads.");
        }
        else {
            sender.sendMessage(
                "Thread selection: {} {} selector{} (matched at aggregation).",
                threads.size(), profiler_.options().regex ? "regex" : "exact-name",
                threads.size() == 1 ? "" : "s");
        }
    }
    else {
        sender.sendMessage("{}Profiler is already running!", ColorFormat::Gold);
    }
    std::int64_t ran = (nowMs() - profiler_.startTimeMs()) / 1000;
    if (allocation) {
        if (profiler_.options().alloc_live_only) {
            sender.sendMessage("So far it has profiled for {} ({} tracked sampled allocations still live process-wide, {} estimated).",
                               spark::formatDuration(ran), profiler_.liveAllocationSamples(),
                               spark::formatBytes(profiler_.liveAllocationBytes()));
        }
        else {
            sender.sendMessage("So far it has profiled for {} ({} selected allocation samples, {} estimated; {} observed process-wide).",
                               spark::formatDuration(ran), profiler_.sampleCount(),
                               spark::formatBytes(profiler_.sampledAllocationBytes()),
                               spark::formatBytes(profiler_.observedAllocationBytes()));
        }
        sender.sendMessage("Process-wide tracked lifecycle: {} freed, {} still live ({}).",
                           profiler_.freedAllocationSamples(),
                           profiler_.liveAllocationSamples(),
                           spark::formatBytes(profiler_.liveAllocationBytes()));
        if (profiler_.droppedSamples() != 0) {
            sender.sendMessage("Dropped allocation samples: {}", profiler_.droppedSamples());
        }
        if (profiler_.filteredAllocationSamples() != 0) {
            sender.sendMessage("Allocation samples excluded by thread selector: {}.",
                               profiler_.filteredAllocationSamples());
        }
        if (profiler_.allocationThreadNameFailures() != 0) {
            sender.sendMessage(
                "Allocation-origin thread names unavailable (failed closed for named selectors): {}.",
                profiler_.allocationThreadNameFailures());
        }
    }
    else {
        sender.sendMessage("So far it has profiled for {} ({} samples).", spark::formatDuration(ran),
                           profiler_.sampleCount());
    }
    std::int64_t auto_end = profiler_.autoEndTimeMs();
    if (auto_end <= 0) {
        sender.sendMessage("To stop and finalize the profile, run: {}/spark profiler stop", ColorFormat::Gray);
    }
    else {
        sender.sendMessage("It finishes automatically in {}.", spark::formatDuration((auto_end - nowMs()) / 1000));
    }
    sender.sendMessage("To cancel without generating a profile, run: {}/spark profiler cancel", ColorFormat::Gray);
}

void ProfilerController::sendAllocationHookCoverage(::endstone::CommandSender &sender)
{
    const auto &capabilities = profiler_.allocationHookCapabilities();
    std::size_t active = 0;
    std::size_t aliases = 0;
    std::string unavailable;
    for (const spark::AllocationHookCapability &capability : capabilities) {
        if (capability.status == spark::AllocationHookStatus::Active) {
            ++active;
        }
        else if (capability.status == spark::AllocationHookStatus::Alias) {
            ++aliases;
        }
        else {
            if (!unavailable.empty()) {
                unavailable += ", ";
            }
            unavailable += capability.name;
            unavailable += '=';
            unavailable += spark::allocationHookStatusName(capability.status);
        }
    }
    sender.sendMessage("Native allocation hooks: {}/{} entry points covered ({} patched targets, {} aliases).",
                       active + aliases, capabilities.size(),
                       profiler_.allocationHookTargetCount(), aliases);
    if (!unavailable.empty()) {
        sender.sendMessage("Unavailable optional hooks: {}", unavailable);
    }
}

void ProfilerController::profilerCancel(::endstone::CommandSender &sender)
{
    if (!profiler_.running()) {
        sender.sendMessage("There isn't an active profiler running.");
        return;
    }
    std::string backend_error;
    const bool failed = profiler_.backendFailure(backend_error);
    std::string error;
    if (!profiler_.cancel(error)) {
        sender.sendMessage("{}Unable to cancel the profiler safely: {}", ColorFormat::Red, error);
        return;
    }
    if (failed) {
        sender.sendMessage("{}Failed allocation profile data was discarded: {}", ColorFormat::Red,
                           backend_error);
        sender.sendMessage("The allocation profiler backend is ready for a new session.");
    }
    else {
        sender.sendMessage("{}Profiler has been cancelled.", ColorFormat::Gold);
    }
}

// Stop and join on the main thread; export and network upload run in the
// background. That task and its main-thread hop capture only `this` so the
// std::function handed to Endstone stays in libc++'s ABI-stable small-buffer form,
// which matters when the plugin and the runtime are built with different libc++.
void ProfilerController::finishProfiler(const std::string &sender_name, bool save,
                                        const std::string &comment)
{
    // Stop before gathering metadata so spark's own world/plugin snapshot
    // allocations do not pollute an allocation profile. Entry hooks remain
    // disabled pass-throughs between sessions; a backend service failure
    // blocks export of the partial data.
    std::string stop_error;
    if (!profiler_.stopSampling(stop_error)) {
        std::string backend_error;
        if (!profiler_.running() && profiler_.backendFailure(backend_error)) {
            announce(sender_name,
                     "Allocation profiler FAILED; incomplete profile data was discarded: " +
                         backend_error);
            announce(sender_name, "The allocation profiler backend is ready for a new session.");
        }
        else {
            announce(sender_name, "Profiler stop failed: " + stop_error);
        }
        return;
    }

    gatherServerInfo(pending_ctx_, plugin_.getServer(), bds_executable_sha256_, nowMs());
    pending_ctx_.comment = comment;
    pending_ctx_.statistics = statistics_.snapshot();
    pending_ctx_.window_stats = statistics_.profileWindows(
        profiler_.startTimeMs(), profiler_.endTimeMs());
    pending_ctx_.system_stats = spark::gatherSystemStats(".");
    gatherWorldInfo(pending_ctx_, plugin_.getServer());

    pending_save_ = save;
    pending_sender_ = sender_name;
    pending_folder_ = spark::profileStorageDirectory(plugin_.getDataFolder());

    exporting_.store(true);
    // NOTE: Endstone's runTaskAsync has a use-after-free - scheduler.cpp submits
    // `[&task]{ task->run(); }`, capturing the loop variable by reference into a
    // detached thread. So we use std::thread directly.
    export_thread_ = std::thread([this]() { runExport(); });
}

void ProfilerController::runExport()
{
    ExportOutcome outcome = ExportOutcome::Failed;
    std::string message;
    try {
        std::string body = profiler_.exportData(pending_ctx_);
        std::string compressed = spark::gzipCompress(body);
        if (pending_save_) {
            spark::ProfileFileResult saved =
                spark::saveProfileToDirectory(pending_folder_, compressed, nowMs());
            if (saved.ok) {
                outcome = ExportOutcome::Saved;
                message = "Saved to " + saved.path.string() + " - open it at " +
                          spark::kViewerUrl;
            }
            else {
                message = "Failed to save the profile: " + saved.error;
            }
        }
        else {
            spark::UploadResult result =
                spark::uploadToBytebin(compressed, spark::kBytebinUrl,
                                       spark::kSamplerContentType,
                                       std::string("endstone-spark/") + spark::kVersion);
            if (result.ok) {
                outcome = ExportOutcome::Uploaded;
                message = std::string(spark::kViewerUrl) + result.key;
            }
            else {
                spark::ProfileFileResult saved =
                    spark::saveProfileToDirectory(pending_folder_, compressed, nowMs());
                if (saved.ok) {
                    outcome = ExportOutcome::Saved;
                    message = "Upload failed (" + result.error +
                              "), so the profile was saved to " + saved.path.string() +
                              " - open it at " + spark::kViewerUrl;
                }
                else {
                    message = "Upload failed (" + result.error +
                              ") and automatic local save failed (" + saved.error + ").";
                }
            }
        }
    }
    catch (const std::exception &e) {
        message = std::string("Export failed: ") + e.what();
    }
    catch (...) {
        message = "Export failed with an unknown error.";
    }
    pending_outcome_ = outcome;
    pending_result_ = std::move(message);
    try {
        plugin_.getServer().getScheduler().runTask(plugin_, [this]() { announceResult(); });
    }
    catch (...) {
        exporting_.store(false);
        throw;
    }
}

// Back on the main thread.
void ProfilerController::announceResult()
{
    const char *headline = pending_outcome_ == ExportOutcome::Uploaded
                               ? "Profiler stopped & upload complete!"
                           : pending_outcome_ == ExportOutcome::Saved
                               ? "Profiler stopped & saved locally!"
                               : "Profiler stopped.";
    announce(pending_sender_, headline);
    announce(pending_sender_, pending_result_);
    exporting_.store(false);
}

void ProfilerController::announce(const std::string &sender_name, const std::string &text)
{
    plugin_.getLogger().info("{}", text);
    auto player = plugin_.getServer().getPlayer(sender_name);
    if (player) {
        player->sendMessage("{}[spark] {}{}", ColorFormat::Gold, ColorFormat::Reset, text);
    }
}

void ProfilerController::onTick(double mspt)
{
    if (!profiler_.running()) {
        return;
    }
    std::string backend_error;
    const bool backend_failed = profiler_.backendFailure(backend_error);
    if (!backend_failed) {
        profiler_.onTick(mspt);
    }
    std::int64_t auto_end = profiler_.autoEndTimeMs();
    if (auto_end > 0 && nowMs() >= auto_end) {
        bool save = profiler_.options().save_to_file;
        finishProfiler(start_sender_name_, save, std::string());
    }
}

}  // namespace spark::endstone_adapter
