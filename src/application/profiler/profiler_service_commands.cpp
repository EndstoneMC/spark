#include <cstdint>
#include <exception>
#include <string>
#include <utility>

#include "application/profiler/profiler_service.h"
#include "application/profiler/profiler_start_options.h"
#include "core/util/base64.h"
#include "core/util/format.h"
#include "core/util/monotonic_time.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return monotonicUnixMillis();
}

}  // namespace

void ProfilerService::cmdStart(CommandSender &sender, const Arguments &args)
{
    if (stopping_.load(std::memory_order_acquire)) {
        sender.sendErrorMessage("The profiler service is stopping.");
        return;
    }
    if (exporting_.load()) {
        sender.sendMessage("The profiler has stopped; results are still being finalized.");
        return;
    }

    auto parsed = spark::parseProfilerStartOptions(args);
    if (!parsed.success()) {
        sender.sendErrorMessage(parsed.error);
        return;
    }
    spark::ProfilerOptions options = std::move(parsed.options);
    options.creator_name = sender.getName();
    options.creator_is_player = sender.isPlayer();
    options.creator_unique_id = options.creator_is_player ? sender.getUniqueId() : std::string{};
    const std::int64_t timeout = options.timeout_seconds;

    std::uint64_t tid = main_tid_;
    if (tid == 0) {
        sender.sendErrorMessage("The server thread hasn't been identified yet - try again in a moment.");
        return;
    }

    if (profiler_.running()) {
        const SessionType interrupted_session = session_type_;
        if (interrupted_session == SessionType::ManualForeground || interrupted_session == SessionType::None) {
            cmdInfo(sender);
            return;
        }
        if (interrupted_session == SessionType::Background) {
            sender.sendMessage("Stopping the background profiler before starting... please wait");
        }
        else {
            sender.sendMessage("Stopping the automatic MSPT profiler before starting... please wait");
        }
        resetProfilerTimeout();
        std::string cancel_error;
        if (!profiler_.cancel(cancel_error)) {
            if (profiler_.running()) {
                restoreProfilerTimeoutAfterFailedStop();
            }
            else {
                if (interrupted_session == SessionType::AutoForeground) {
                    beginAutoProfilerCooldown();
                }
                session_type_ = SessionType::None;
                background_started_ = false;
            }
            if (interrupted_session == SessionType::Background) {
                sender.sendErrorMessage("Couldn't stop the background profiler safely: {}", cancel_error);
            }
            else {
                sender.sendErrorMessage("Couldn't stop the automatic MSPT profiler safely: {}", cancel_error);
            }
            return;
        }
        if (interrupted_session == SessionType::AutoForeground) {
            beginAutoProfilerCooldown();
        }
        session_type_ = SessionType::None;
        background_started_ = false;
    }

    const auto replacement_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    const bool timer_stopped = resetProfilerTimeoutUntil(replacement_deadline);
    const bool viewer_stopped = !viewer_open_ || viewer_open_->retireUntil(replacement_deadline);
    if (!timer_stopped || !viewer_stopped) {
        sender.sendErrorMessage(!timer_stopped ? "previous profiler timer still stopping; retry"
                                               : "previous live viewer still closing; retry");
        return;
    }
    resetProfilerTimeout();
    std::vector<NativePluginSource> native_plugin_sources;
    try {
        native_plugin_sources = metadata_provider_.nativePluginSources();
    }
    catch (const std::exception &error) {
        session_type_ = SessionType::None;
        background_started_ = false;
        background_retry_delay_s_ = 0;
        next_background_retry_ms_ = 0;
        sender.sendErrorMessage("Couldn't start the profiler: metadata collection failed ({})", error.what());
        return;
    }
    catch (...) {
        session_type_ = SessionType::None;
        background_started_ = false;
        background_retry_delay_s_ = 0;
        next_background_retry_ms_ = 0;
        sender.sendErrorMessage("Couldn't start the profiler: metadata collection failed");
        return;
    }
    std::string error;
    bool started = false;
    try {
        started = profiler_.start(options, tid, error);
    }
    catch (const std::exception &exception) {
        error = exception.what();
    }
    catch (...) {
        error = "unknown profiler startup failure";
    }
    if (!started) {
        if (profiler_.running()) {
            try {
                profiler_.cancel();
            }
            catch (...) {  // NOLINT(bugprone-empty-catch): startup cleanup is best effort.
            }
        }
        session_type_ = SessionType::None;
        background_started_ = false;
        background_retry_delay_s_ = 0;
        next_background_retry_ms_ = 0;
        sender.sendErrorMessage("Couldn't start the profiler: {}", error);
        return;
    }
    session_native_plugin_sources_ = std::move(native_plugin_sources);
    start_sender_name_ = sender.getName();
    start_sender_is_player_ = sender.isPlayer();
    start_sender_unique_id_ = start_sender_is_player_ ? sender.getUniqueId() : std::string{};
    session_type_ = SessionType::ManualForeground;
    background_started_ = false;

    if (timeout > 0 && !armProfilerTimeout(timeout)) {
        resetProfilerTimeout();
        std::string cancel_error;
        const bool cancelled = profiler_.cancel(cancel_error);
        if (!cancelled) {
            restoreProfilerTimeoutAfterFailedStop();
        }
        session_type_ = SessionType::None;
        background_started_ = false;
        if (!cancelled && !cancel_error.empty()) {
            sender.sendErrorMessage("Couldn't start the profiler: timeout setup failed ({}); cleanup failed: {}",
                                    timeout, cancel_error);
        }
        else {
            sender.sendErrorMessage("Couldn't start the profiler: timeout setup failed");
        }
        return;
    }

    if (options.alloc) {
        if (options.alloc_live_only) {
            sender.sendMessage("{}Retained Allocation Profiler is now running!{} (async)", kColorGold, kColorGray);
        }
        else {
            sender.sendMessage("{}Allocation Profiler is now running!{} (async)", kColorGold, kColorGray);
        }
        if (options.threads.empty() || (options.threads.size() == 1 && options.threads.front() == "*")) {
            sender.sendMessage("Sampling approximately every {} of native allocations across process threads.",
                               spark::formatBytes(static_cast<std::uint64_t>(options.allocation_interval_bytes)));
        }
        else {
            sender.sendMessage("Sampling approximately every {} of native allocations from matching threads.",
                               spark::formatBytes(static_cast<std::uint64_t>(options.allocation_interval_bytes)));
        }
        if (options.alloc_live_only) {
            sender.sendMessage("The result will contain only sampled allocations still live when profiling stops.");
        }
    }
    else {
        if (options.threads.empty()) {
            sender.sendMessage("{}Profiler is now running!{} (async, {}ms interval)", kColorGold, kColorGray,
                               options.interval_ms);
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
        sender.sendMessage("To view the profile while it runs, run: {}/spark profiler open", kColorGray);
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
        const SessionType stopped_session = session_type_;
        session_type_ = SessionType::None;
        background_started_ = false;
        if (stopped_session == SessionType::AutoForeground) {
            beginAutoProfilerCooldown();
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
    resetProfilerTimeout();
    closeViewerSocket();
    const bool sender_is_player = sender.isPlayer();
    finishProfiler(sender.getName(), sender_is_player, sender_is_player ? sender.getUniqueId() : std::string{}, save,
                   comment);
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
        sender.sendMessage("Run {}/spark profiler stop{} or {}/spark profiler cancel{} to discard it.", kColorGray,
                           kColorReset, kColorGray, kColorReset);
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
        if (threads.empty() || (threads.size() == 1 && threads.front() == "*")) {
            sender.sendMessage("Thread selection: all process threads.");
        }
        else {
            sender.sendMessage("Thread selection: {} {} selector{} (matched at aggregation).", threads.size(),
                               profiler_.options().regex ? "regex" : "exact-name", threads.size() == 1 ? "" : "s");
        }
    }
    else {
        sender.sendMessage("{}Profiler is already running!", kColorGold);
    }
    std::int64_t ran = (nowMs() - profiler_.startTimeMs()) / 1000;
    if (!allocation && session_type_ == SessionType::Background) {
        sender.sendMessage("It was started automatically when spark enabled and has been "
                           "running in the background for {}.",
                           spark::formatDuration(ran));
    }
    else if (!allocation && session_type_ == SessionType::AutoForeground) {
        sender.sendMessage("It was started automatically after sustained high MSPT.");
    }
    if (allocation) {
        if (profiler_.options().alloc_live_only) {
            sender.sendMessage(
                "So far it has profiled for {} ({} tracked sampled allocations still live process-wide, {} estimated).",
                spark::formatDuration(ran), profiler_.liveAllocationSamples(),
                spark::formatBytes(profiler_.liveAllocationBytes()));
        }
        else {
            sender.sendMessage("So far it has profiled for {} ({} selected allocation samples, {} estimated; {} "
                               "observed process-wide).",
                               spark::formatDuration(ran), profiler_.sampleCount(),
                               spark::formatBytes(profiler_.sampledAllocationBytes()),
                               spark::formatBytes(profiler_.observedAllocationBytes()));
        }
        sender.sendMessage("Process-wide tracked lifecycle: {} freed, {} still live ({}).",
                           profiler_.freedAllocationSamples(), profiler_.liveAllocationSamples(),
                           spark::formatBytes(profiler_.liveAllocationBytes()));
        if (profiler_.droppedSamples() != 0) {
            sender.sendMessage("Dropped allocation samples: {}", profiler_.droppedSamples());
        }
        if (profiler_.filteredAllocationSamples() != 0) {
            sender.sendMessage("Allocation samples excluded by thread selector: {}.",
                               profiler_.filteredAllocationSamples());
        }
        if (profiler_.allocationThreadNameFailures() != 0) {
            sender.sendMessage("Allocation-origin thread names unavailable (failed closed for named selectors): {}.",
                               profiler_.allocationThreadNameFailures());
        }
    }
    else {
        sender.sendMessage("So far it has profiled for {} ({} samples).", spark::formatDuration(ran),
                           profiler_.sampleCount());
        if (profiler_.droppedSamples() != 0) {
            sender.sendMessage("Dropped execution samples: {}", profiler_.droppedSamples());
        }
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
                       active + aliases, capabilities.size(), profiler_.allocationHookTargetCount(), aliases);
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
    const SessionType cancelled_session = session_type_;
    const auto settle_cancelled_session = [this, cancelled_session]() noexcept {
        session_type_ = SessionType::None;
        background_started_ = false;
        if (cancelled_session == SessionType::Background) {
            // Cancelling the background session is an explicit user pause. Foreground
            // cancellation is only a temporary interruption and must not suppress it.
            background_suppressed_ = background_enabled_;
        }
        else if (cancelled_session == SessionType::AutoForeground) {
            beginAutoProfilerCooldown();
        }
        closeViewerSocket();
    };

    resetProfilerTimeout();
    std::string error;
    if (!profiler_.cancel(error)) {
        if (profiler_.running()) {
            restoreProfilerTimeoutAfterFailedStop();
        }
        else {
            settle_cancelled_session();
        }
        sender.sendMessage("{}Unable to cancel the profiler safely: {}", kColorRed, error);
        return;
    }
    settle_cancelled_session();
    if (failed) {
        sender.sendMessage("{}Failed allocation profile data was discarded: {}", kColorRed, backend_error);
        sender.sendMessage("The allocation profiler backend is ready for a new session.");
    }
    else {
        sender.sendMessage("{}Profiler has been cancelled.", kColorGold);
    }
}

void ProfilerService::cmdOpen(CommandSender &sender, const Arguments &args)
{
    if (stopping_.load(std::memory_order_acquire)) {
        sender.sendErrorMessage("The profiler service is stopping.");
        return;
    }
    if (viewer_open_) {
        viewer_open_->cmdOpen(sender, args);
    }
}

void ProfilerService::cmdTrustViewer(CommandSender &sender, const Arguments &args)
{
    auto ids = args.stringFlag("id");
    if (ids.empty()) {
        sender.sendMessage("Usage: /spark profiler trust-viewer --id <client id>");
        sender.sendMessage("Use the client id shown when a viewer connects.");
        return;
    }
    const auto viewer_socket = viewer_open_ ? viewer_open_->viewerSocket() : nullptr;
    if (!viewer_socket || !viewer_socket->isOpen()) {
        sender.sendMessage("No live viewer is currently open.");
        return;
    }
    for (const auto &id : ids) {
        auto key = viewer_socket->pendingKey(id);
        if (key.empty()) {
            sender.sendMessage("No pending client found with id '{}'.", id);
            continue;
        }
        std::string b64 = base64Encode(key.data(), key.size());
        // Avoid duplicates.
        if (trusted_viewers_.contains(b64)) {
            sender.sendMessage("Client '{}' is already trusted.", id);
            continue;
        }
        if (!trusted_viewers_.addAndSave(b64)) {
            const std::string error = trusted_viewers_.lastError();
            if (error.empty()) {
                sender.sendErrorMessage("Unable to persist trust for client '{}'.", id);
            }
            else {
                sender.sendErrorMessage("Unable to persist trust for client '{}': {}", id, error);
            }
            continue;
        }
        viewer_socket->sendClientTrusted(id);
        sender.sendMessage("Client '{}' is now trusted.", id);
    }
}

}  // namespace spark
