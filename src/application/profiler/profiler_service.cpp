#include "application/profiler/profiler_service.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "core/util/format.h"
#include "core/stats/system_stats.h"
#include "spark_constants.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ProfilerService::ProfilerService(StatisticsService &statistics,
                                 std::string bds_executable_sha256,
                                 std::filesystem::path profile_storage_dir,
                                 MainThreadDispatcher &dispatcher,
                                 ProfileMetadataProvider &metadata_provider,
                                 ResultNotifier &notifier)
    : statistics_(statistics),
      bds_executable_sha256_(std::move(bds_executable_sha256)),
      dispatcher_(dispatcher),
      metadata_provider_(metadata_provider),
      notifier_(notifier),
      exporter_(std::move(profile_storage_dir))
{
}

ProfilerService::~ProfilerService()
{
    if (export_thread_.joinable()) {
        export_thread_.join();
    }
}

void ProfilerService::shutdown()
{
    if (export_thread_.joinable()) {
        export_thread_.join();
    }
}

void ProfilerService::cmdStart(CommandSender &sender, const Arguments &args)
{
    if (profiler_.running()) {
        cmdInfo(sender);
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
    if (args.boolFlag("combine-all") && args.boolFlag("not-combined")) {
        sender.sendErrorMessage("--combine-all and --not-combined cannot be used together.");
        return;
    }
    if (args.boolFlag("combine-all")) {
        options.thread_grouper = spark::ThreadGrouperMode::AsOne;
    } else if (args.boolFlag("not-combined")) {
        options.thread_grouper = spark::ThreadGrouperMode::ByName;
    }
    auto comments = args.stringFlag("comment");
    if (!comments.empty()) {
        options.comment = comments.front();
    }
    options.save_to_file = args.boolFlag("save-to-file");
    options.creator_name = sender.getName();
    options.creator_is_player = sender.isPlayer();

    std::uint64_t tid = main_tid_;
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
    start_sender_is_player_ = sender.isPlayer();

    if (options.alloc) {
        if (options.alloc_live_only) {
            sender.sendMessage("{}Retained Allocation Profiler is now running!{} (async)",
                               kColorGold, kColorGray);
        }
        else {
            sender.sendMessage("{}Allocation Profiler is now running!{} (async)",
                               kColorGold, kColorGray);
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
            sender.sendMessage("{}Profiler is now running!{} (async, {}ms interval)", kColorGold,
                               kColorGray, options.interval_ms);
        }
        else if (options.threads.size() == 1 && options.threads.front() == "*") {
            sender.sendMessage("{}Profiler is now running for all process threads!{} (async, {}ms interval)",
                               kColorGold, kColorGray, options.interval_ms);
        }
        else {
            sender.sendMessage("{}Profiler is now running for selected process threads!{} (async, {}ms interval)",
                               kColorGold, kColorGray, options.interval_ms);
        }
    }
    if (options.only_ticks_over_ms > 0) {
        sender.sendMessage("Only recording ticks longer than {}ms.", options.only_ticks_over_ms);
    }
    if (timeout <= 0) {
        sender.sendMessage("It runs in the background until stopped.");
        sender.sendMessage("To stop and finalize the profile, run: {}/spark profiler stop", kColorGray);
    }
    else {
        if (timeout < 30) {
            sender.sendMessage("Tip: a timeout over 30s gives noticeably more accurate results.");
        }
        sender.sendMessage("Results will be returned automatically after {}.", spark::formatDuration(timeout));
    }
}

void ProfilerService::cmdStop(CommandSender &sender, const Arguments &args)
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
            sender.sendMessage("{}Allocation profiler status: FAILED", kColorRed);
            sender.sendMessage("Unable to discard the failed session safely: {}", cleanup_error);
            return;
        }
        sender.sendMessage("{}Allocation profiler status: FAILED", kColorRed);
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
    sender.sendMessage("{}Stopping the profiler and finalizing results, please wait...", kColorGold);
    finishProfiler(sender.getName(), sender.isPlayer(), save, comment);
}

void ProfilerService::cmdInfo(CommandSender &sender)
{
    if (!profiler_.running()) {
        if (exporting_.load()) {
            sender.sendMessage("The profiler has stopped; results are still being finalized.");
            return;
        }
        sender.sendMessage("The profiler isn't running!");
        sender.sendMessage("To start a new one, run: {}/spark profiler start", kColorGray);
        return;
    }
    const bool allocation = profiler_.mode() == spark::ProfileMode::Allocation;
    std::string backend_error;
    if (allocation && profiler_.backendFailure(backend_error)) {
        sender.sendMessage("{}Allocation Profiler status: FAILED", kColorRed);
        sender.sendMessage("Backend service failure: {}", backend_error);
        sendAllocationHookCoverage(sender);
        sender.sendMessage("The incomplete profile will not be exported.");
        sender.sendMessage("Run {}/spark profiler stop{} or {}/spark profiler cancel{} to discard it.",
                           kColorGray, kColorReset, kColorGray, kColorReset);
        return;
    }
    if (allocation) {
        if (profiler_.options().alloc_live_only) {
            sender.sendMessage("{}Retained Allocation Profiler is already running!", kColorGold);
        }
        else {
            sender.sendMessage("{}Allocation Profiler is already running!", kColorGold);
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
        sender.sendMessage("{}Profiler is already running!", kColorGold);
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
        sender.sendMessage("To stop and finalize the profile, run: {}/spark profiler stop", kColorGray);
    }
    else {
        sender.sendMessage("It finishes automatically in {}.", spark::formatDuration((auto_end - nowMs()) / 1000));
    }
    sender.sendMessage("To cancel without generating a profile, run: {}/spark profiler cancel", kColorGray);
}

void ProfilerService::sendAllocationHookCoverage(CommandSender &sender)
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

void ProfilerService::cmdCancel(CommandSender &sender)
{
    if (!profiler_.running()) {
        sender.sendMessage("There isn't an active profiler running.");
        return;
    }
    std::string backend_error;
    const bool failed = profiler_.backendFailure(backend_error);
    std::string error;
    if (!profiler_.cancel(error)) {
        sender.sendMessage("{}Unable to cancel the profiler safely: {}", kColorRed, error);
        return;
    }
    if (failed) {
        sender.sendMessage("{}Failed allocation profile data was discarded: {}", kColorRed,
                           backend_error);
        sender.sendMessage("The allocation profiler backend is ready for a new session.");
    }
    else {
        sender.sendMessage("{}Profiler has been cancelled.", kColorGold);
    }
}

void ProfilerService::finishProfiler(const std::string &sender_name, bool sender_is_player,
                                     bool save, const std::string &comment)
{
    std::string stop_error;
    if (!profiler_.stopSampling(stop_error)) {
        std::string backend_error;
        if (!profiler_.running() && profiler_.backendFailure(backend_error)) {
            notifier_.notify(sender_name,
                     "Allocation profiler FAILED; incomplete profile data was discarded: " +
                         backend_error);
            notifier_.notify(sender_name, "The allocation profiler backend is ready for a new session.");
        }
        else {
            notifier_.notify(sender_name, "Profiler stop failed: " + stop_error);
        }
        return;
    }

    pending_ctx_ = ExportContext{};
    pending_ctx_.bds_executable_sha256 = bds_executable_sha256_;
    metadata_provider_.gatherServerMetadata(pending_ctx_, nowMs());
    pending_ctx_.comment = comment;
    pending_ctx_.statistics = statistics_.snapshot();
    pending_ctx_.window_stats = statistics_.profileWindows(
        profiler_.startTimeMs(), profiler_.endTimeMs());
    pending_ctx_.system_stats = spark::gatherSystemStats(".");
    metadata_provider_.gatherWorldMetadata(pending_ctx_);
    if (ping_samples_provider_) {
        pending_ctx_.ping_samples = ping_samples_provider_();
    }
    if (network_snapshot_provider_) {
        pending_ctx_.net_snapshots = network_snapshot_provider_();
    }

    pending_save_ = save;
    pending_sender_ = sender_name;
    pending_sender_is_player_ = sender_is_player;

    exporting_.store(true);
    export_thread_ = std::thread([this]() { runExport(); });
}

void ProfilerService::runExport()
{
    ProfileExporter::Result result =
        exporter_.exportProfile(profiler_, pending_ctx_, pending_save_);
    pending_outcome_ = result.outcome;
    pending_result_ = std::move(result.message);
    try {
        dispatcher_.runOnMainThread([this]() { announceResult(); });
    }
    catch (...) {
        exporting_.store(false);
        throw;
    }
}

void ProfilerService::announceResult()
{
    const char *headline = pending_outcome_ == ExportOutcome::Uploaded
                               ? "Profiler stopped & upload complete!"
                           : pending_outcome_ == ExportOutcome::Saved
                               ? "Profiler stopped & saved locally!"
                               : "Profiler stopped.";
    notifier_.notify(pending_sender_, headline);
    notifier_.notify(pending_sender_, pending_result_);

    if (activity_log_provider_) {
        ActivityLog *log = activity_log_provider_();
        if (log) {
            const std::int64_t now_ms = nowMs();
            if (pending_outcome_ == ExportOutcome::Uploaded) {
                log->add(Activity::url(pending_sender_, pending_sender_is_player_, now_ms,
                                       "Profiler", pending_result_));
            } else if (pending_outcome_ == ExportOutcome::Saved) {
                log->add(Activity::file(pending_sender_, pending_sender_is_player_, now_ms,
                                        "Profiler", pending_result_));
            }
        }
    }

    exporting_.store(false);
}

void ProfilerService::onTick(double mspt)
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
        finishProfiler(start_sender_name_, start_sender_is_player_, save, std::string());
    }
}

}  // namespace spark
