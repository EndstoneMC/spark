#include "core/profiler/profiler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <regex>
#include <stdexcept>

#include "core/util/monotonic_time.h"
#include "native/diagnostics/ci_diagnostics.h"
#include "profiling_window.h"
#include "spark_constants.h"

namespace spark {
namespace {

std::int64_t nowMs()
{
    return monotonicUnixMillis();
}

std::uint64_t saturatingAdd(std::uint64_t left, std::uint64_t right) noexcept
{
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return left > maximum - right ? maximum : left + right;
}

}  // namespace

std::uint64_t Profiler::sampleCount() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.sampleCount() : sampler_.sampleCount();
}

std::uint64_t Profiler::sampledAllocationBytes() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.sampledBytes() : 0;
}

std::uint64_t Profiler::observedAllocationBytes() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.observedBytes() : 0;
}

std::uint64_t Profiler::droppedSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.droppedSamples() : sampler_.droppedSamples();
}

std::uint64_t Profiler::filteredAllocationSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.filteredSamples() : 0;
}

std::uint64_t Profiler::allocationThreadNameFailures() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.threadNameFailures() : 0;
}

std::uint64_t Profiler::freedAllocationSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.freedSamples() : 0;
}

std::uint64_t Profiler::liveAllocationSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.liveSamples() : 0;
}

std::uint64_t Profiler::liveAllocationBytes() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.liveBytes() : 0;
}

bool Profiler::backendFailure(std::string &error) const
{
    if (mode_ == ProfileMode::Execution) {
        return sampler_.failure(error);
    }
    return allocation_sampler_.failure(error);
}

const std::vector<AllocationHookCapability> &Profiler::allocationHookCapabilities() const
{
    return allocation_sampler_.hookCapabilities();
}

bool Profiler::setCurrentThreadAllocationTrackingSuppressed(bool suppressed) noexcept
{
    return allocation_sampler_.setCurrentThreadTrackingSuppressed(suppressed);
}

std::size_t Profiler::allocationHookTargetCount() const
{
    return allocation_sampler_.hookTargetCount();
}

void Profiler::accumulatePersistentAllocationBytes() noexcept
{
    const std::uint64_t current = persistent_allocation_bytes_base_.load(std::memory_order_relaxed);
    persistent_allocation_bytes_base_.store(saturatingAdd(current, allocation_sampler_.observedBytes()),
                                            std::memory_order_release);
}

bool Profiler::startPersistentAllocationCounting(std::string &error)
{
    error.clear();
    if (allocation_sampler_.backendCleanupPending()) {
        error = "the allocation backend is still finishing cleanup";
        return false;
    }
    if (persistent_allocation_counting_active_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!persistent_allocation_counting_enabled_.load(std::memory_order_acquire) ||
        persistent_allocation_session_seed_ == 0) {
        error = "persistent allocation counting is not configured";
        return false;
    }
    AllocationSamplerConfig config;
    config.interval_bytes = kDefaultAllocationIntervalBytes;
    config.session_seed = persistent_allocation_session_seed_;
    config.all_threads = true;
    config.count_only = true;
    allocation_sampler_.setRecoverySink(nullptr);
    if (!allocation_sampler_.start(config, error)) {
        return false;
    }
    persistent_allocation_stop_accumulated_.store(false, std::memory_order_release);
    persistent_allocation_counting_active_.store(true, std::memory_order_release);
    return true;
}

bool Profiler::stopPersistentAllocationCounting(std::string &error)
{
    error.clear();
    const bool active = persistent_allocation_counting_active_.load(std::memory_order_acquire);
    if (!active && !allocation_sampler_.backendCleanupPending()) {
        return true;
    }
    if (!allocation_sampler_.stop(error)) {
        if (!allocation_sampler_.running() && !allocation_sampler_.backendCleanupPending()) {
            if (!persistent_allocation_stop_accumulated_.exchange(true, std::memory_order_acq_rel)) {
                accumulatePersistentAllocationBytes();
            }
            persistent_allocation_counting_active_.store(false, std::memory_order_release);
        }
        return false;
    }
    if (!persistent_allocation_stop_accumulated_.exchange(true, std::memory_order_acq_rel)) {
        accumulatePersistentAllocationBytes();
    }
    persistent_allocation_counting_active_.store(false, std::memory_order_release);
    return true;
}

bool Profiler::setPersistentAllocationCountingEnabled(bool enabled, std::uint64_t session_seed, std::string &error)
{
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    error.clear();
    if (!enabled) {
        if (!stopPersistentAllocationCounting(error)) {
            return false;
        }
        persistent_allocation_counting_enabled_.store(false, std::memory_order_release);
        persistent_allocation_session_seed_ = 0;
        return true;
    }
    if (session_seed == 0) {
        error = "the allocation rate session seed is not available";
        return false;
    }
    persistent_allocation_session_seed_ = session_seed;
    persistent_allocation_counting_enabled_.store(true, std::memory_order_release);
    // Enabling metrics must not reset either a running full allocation profile or
    // a completed profile that has not reached the serialization/discard hand-off.
    if (allocation_export_pending_.load(std::memory_order_acquire) ||
        (running_.load(std::memory_order_acquire) && mode_ == ProfileMode::Allocation)) {
        return true;
    }
    if (!startPersistentAllocationCounting(error)) {
        persistent_allocation_counting_enabled_.store(false, std::memory_order_release);
        persistent_allocation_session_seed_ = 0;
        return false;
    }
    return true;
}

std::uint64_t Profiler::persistentAllocationBytes() const
{
    if (!persistent_allocation_counting_enabled_.load(std::memory_order_acquire)) {
        return 0;
    }
    const std::uint64_t base = persistent_allocation_bytes_base_.load(std::memory_order_acquire);
    const bool current_session_counts = persistent_allocation_counting_active_.load(std::memory_order_acquire) ||
                                        (running_.load(std::memory_order_acquire) && mode_ == ProfileMode::Allocation);
    return current_session_counts ? saturatingAdd(base, allocation_sampler_.observedBytes()) : base;
}

void Profiler::requestStop() noexcept
{
    sampling_stop_requested_.store(true, std::memory_order_release);
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    if (mode_ == ProfileMode::Allocation) {
        allocation_sampler_.requestStop();
    }
    else {
        sampler_.requestStop();
    }
}

bool Profiler::start(const ProfilerOptions &options, std::uint64_t main_tid, std::string &error)
{
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    CiDiagnostics *diagnostics = globalCiDiagnostics();
    if (running_.load()) {
        error = "profiler is already running";
        return false;
    }
    if (retain_recovery_journal_on_shutdown_.load(std::memory_order_acquire)) {
        error = "the previous recovery journal is retained until export cleanup completes";
        return false;
    }
    if (!reapRecoveryWriter()) {
        if (allocation_sampler_.backendCleanupPending()) {
            error = "the allocation backend is still finishing cleanup";
        }
        else {
            error = allocation_sampler_.aggregatorMayBeAlive() ? "the allocation aggregator is still finishing"
                                                               : "previous recovery writer is still stopping";
        }
        return false;
    }
    if (allocation_export_pending_.load(std::memory_order_acquire)) {
        error = "previous allocation profile is still awaiting export or discard";
        return false;
    }
    sampling_stop_requested_.store(false, std::memory_order_release);
    included_ticks_.store(0, std::memory_order_relaxed);

    if (options.regex && options.threads.empty()) {
        error = "--regex requires at least one --thread pattern";
        return false;
    }
    if (std::ranges::find(options.threads, "*") != options.threads.end() &&
        (options.regex || options.threads.size() != 1)) {
        error = "--thread * cannot be combined with other thread selectors or --regex";
        return false;
    }

    options_ = options;
    mode_ = options.alloc ? ProfileMode::Allocation : ProfileMode::Execution;

    const std::int64_t session_start_ms = nowMs();
    if (options.timeout_seconds > 0 &&
        options.timeout_seconds > (std::numeric_limits<std::int64_t>::max() - session_start_ms) / 1000) {
        error = "profiling timeout is too large";
        return false;
    }
    if (options.only_ticks_over_ms > std::numeric_limits<std::int32_t>::max()) {
        error = "tick threshold is too large";
        return false;
    }

    bool started = false;
    if (mode_ == ProfileMode::Allocation) {
        if (diagnostics != nullptr) {
            diagnostics->beginGeneration();
        }
        if (options.allocation_interval_bytes <= 0) {
            error = "allocation sampling interval must be greater than zero";
            return false;
        }
        interval_ = options.allocation_interval_bytes;
        AllocationSamplerConfig config;
        config.interval_bytes = options.allocation_interval_bytes;
        config.session_seed = main_tid;
        config.only_ticks_over_ms = options.only_ticks_over_ms > 0 ? options.only_ticks_over_ms : 0;
        config.all_threads = options.threads.empty() || (options.threads.size() == 1 && options.threads.front() == "*");
        config.regex_threads = options.regex;
        if (!config.all_threads) {
            config.thread_patterns = options.threads;
        }
        config.live_only = options.alloc_live_only;
        config.fail_aggregator_for_testing = options.fail_allocation_aggregator_for_testing;
        config.aggregator_delay_ms_for_testing = options.allocation_aggregator_delay_ms_for_testing;
        if (allocation_sampler_.backendCleanupPending()) {
            error = "the allocation backend is still finishing cleanup";
            return false;
        }
        if (persistent_allocation_counting_active_.load(std::memory_order_acquire) &&
            !stopPersistentAllocationCounting(error)) {
            return false;
        }
        persistent_allocation_stop_accumulated_.store(false, std::memory_order_release);
        if (!recovery_dir_.empty()) {
            RecoveryWriter::Config wc;
            wc.directory = recovery_dir_;
            wc.session_id = static_cast<std::uint64_t>(session_start_ms);
            auto writer = std::make_unique<RecoveryWriter>(std::move(wc));
            if (writer->start()) {
                writer->journalSessionConfig(
                    static_cast<std::uint32_t>(interval_),
                    options.only_ticks_over_ms > 0 ? static_cast<std::int32_t>(options.only_ticks_over_ms) : 0,
                    config.all_threads, config.regex_threads, false, static_cast<std::uint8_t>(options.thread_grouper),
                    1, config.live_only, options.creator_name, options.creator_is_player, options.comment,
                    options.threads, profiling_window::windowAdjustmentMs(), options.creator_unique_id);
                writer->requestFlush();
                std::scoped_lock lock(recovery_mutex_);
                recovery_writer_ = std::move(writer);
                allocation_sampler_.setRecoverySink(recovery_writer_.get());
                journal_degradation_active_.store(true, std::memory_order_release);
                journal_degradation_logged_.store(false, std::memory_order_release);
            }
            else {
                allocation_sampler_.setRecoverySink(nullptr);
            }
        }
        started = allocation_sampler_.start(config, error);
    }
    else {
        const int interval_ms = options.interval_ms > 0 ? options.interval_ms : 4;
        if (interval_ms > kMaxSamplingIntervalMs) {
            error = "sampling interval must not exceed 1000 milliseconds";
            return false;
        }
        interval_ = interval_ms * 1000;

        SamplerConfig config;
        config.interval_us = interval_;
        config.ignore_sleeping = options.ignore_sleeping;
        config.all_threads = options.threads.size() == 1 && options.threads.front() == "*";
        config.regex_threads = options.regex;
        if (!config.all_threads) {
            config.thread_patterns = options.threads;
        }
        config.only_ticks_over_ms = options.only_ticks_over_ms > 0 ? options.only_ticks_over_ms : 0;
        sampler_.setTarget(main_tid);
        if (!recovery_dir_.empty()) {
            RecoveryWriter::Config wc;
            wc.directory = recovery_dir_;
            wc.session_id = static_cast<std::uint64_t>(session_start_ms);
            auto writer = std::make_unique<RecoveryWriter>(std::move(wc));
            if (writer->start()) {
                writer->journalSessionConfig(
                    static_cast<std::uint32_t>(interval_),
                    options.only_ticks_over_ms > 0 ? static_cast<std::int32_t>(options.only_ticks_over_ms) : 0,
                    config.all_threads, config.regex_threads, config.ignore_sleeping,
                    static_cast<std::uint8_t>(options.thread_grouper), 0, false, options.creator_name,
                    options.creator_is_player, options.comment, options.threads, profiling_window::windowAdjustmentMs(),
                    options.creator_unique_id);
                // Journal the bounded execution module sentinel before samples.
                writer->journalModuleDef(0, kOtherModulesSentinel);
                writer->requestFlush();
                std::scoped_lock lock(recovery_mutex_);
                recovery_writer_ = std::move(writer);
                sampler_.setRecoverySink(recovery_writer_.get());
                journal_degradation_active_.store(true, std::memory_order_release);
                journal_degradation_logged_.store(false, std::memory_order_release);
            }
            else {
                sampler_.setRecoverySink(nullptr);
            }
        }
        started = sampler_.start(config);
        if (!started) {
            error = sampler_.lastError().empty() ? "the platform stack-capture backend could not be initialized"
                                                 : sampler_.lastError();
        }
    }

    if (!started) {
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Profiler, CiDiagnosticPhase::ProfilerStartFailed, main_tid);
        }
        stopRecoveryWriter();
        if (mode_ == ProfileMode::Allocation &&
            persistent_allocation_counting_enabled_.load(std::memory_order_acquire)) {
            std::string resume_error;
            startPersistentAllocationCounting(resume_error);
        }
        return false;
    }

    running_.store(true);
    start_time_ms_ = session_start_ms;
    end_time_ms_ = 0;
    auto_end_time_ms_ =
        options.timeout_seconds > 0 ? start_time_ms_ + static_cast<std::int64_t>(options.timeout_seconds) * 1000 : -1;
    if (diagnostics != nullptr) {
        diagnostics->publish(CiDiagnosticContext::Profiler, CiDiagnosticPhase::ProfilerStart, main_tid);
    }
    return true;
}

void Profiler::onTick(double mspt_ms)
{
    if (persistent_allocation_counting_enabled_.load(std::memory_order_acquire) &&
        !persistent_allocation_counting_active_.load(std::memory_order_acquire) &&
        !allocation_export_pending_.load(std::memory_order_acquire)) {
        std::string ignored;
        resumePersistentAllocationCounting(ignored);
    }
    if (persistent_allocation_counting_active_.load(std::memory_order_acquire)) {
        allocation_sampler_.onTick(mspt_ms);
        if (!allocation_sampler_.running()) {
            // This is an exceptional count-only backend stop. Serialize it with
            // lifecycle operations so observed bytes are accumulated exactly once
            // before a later restart resets the session counters.
            std::scoped_lock lifecycle_lock(lifecycle_mutex_);
            if (persistent_allocation_counting_active_.load(std::memory_order_acquire) &&
                !allocation_sampler_.running()) {
                std::string ignored;
                stopPersistentAllocationCounting(ignored);
            }
        }
    }
    if (!running_.load() || sampling_stop_requested_.load(std::memory_order_acquire)) {
        return;
    }
    if (options_.only_ticks_over_ms > 0 && std::isfinite(mspt_ms) &&
        mspt_ms > static_cast<double>(options_.only_ticks_over_ms)) {
        std::int32_t current = included_ticks_.load(std::memory_order_relaxed);
        while (current < std::numeric_limits<std::int32_t>::max() &&
               !included_ticks_.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
        }
    }
    if (mode_ == ProfileMode::Allocation) {
        allocation_sampler_.onTick(mspt_ms);
    }
    else {
        sampler_.onTick(mspt_ms);
    }
}

bool Profiler::stopSampling(std::string &error)
{
    CiDiagnostics *diagnostics = globalCiDiagnostics();
    CiDiagnostics::Scope diagnostic_scope(
        diagnostics, CiDiagnosticContext::Profiler, CiDiagnosticPhase::ProfilerStopSamplingEnter,
        CiDiagnosticPhase::ProfilerStopSamplingExit, CiDiagnosticPhase::ProfilerStopSamplingExceptionalExit,
        ciDiagnosticCurrentThreadId());
    sampling_stop_requested_.store(true, std::memory_order_release);
    if (stop_requested_hook_) {
        stop_requested_hook_();
    }
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    error.clear();
    if (!running_.load()) {
        return true;
    }
    const std::int64_t requested_end_time_ms = nowMs();
    if (mode_ == ProfileMode::Allocation) {
        if (!allocation_sampler_.stop(error)) {
            if (!allocation_sampler_.running()) {
                if (persistent_allocation_counting_enabled_.load(std::memory_order_acquire) &&
                    !allocation_sampler_.backendCleanupPending() &&
                    !persistent_allocation_stop_accumulated_.exchange(true, std::memory_order_acq_rel)) {
                    accumulatePersistentAllocationBytes();
                }
                allocation_export_pending_.store(true, std::memory_order_release);
                running_.store(false);
                end_time_ms_ = requested_end_time_ms;
            }
            return false;
        }
        if (persistent_allocation_counting_enabled_.load(std::memory_order_acquire) &&
            !persistent_allocation_stop_accumulated_.exchange(true, std::memory_order_acq_rel)) {
            accumulatePersistentAllocationBytes();
        }
        stopRecoveryWriter();
        allocation_export_pending_.store(true, std::memory_order_release);
    }
    else {
        if (!sampler_.stop()) {
            error = sampler_.lastError();
            return false;
        }
        stopRecoveryWriter();
        if (sampler_.failure(error)) {
            running_.store(false);
            end_time_ms_ = requested_end_time_ms;
            return false;
        }
    }
    running_.store(false);
    end_time_ms_ = requested_end_time_ms;
    // Do not restart persistent count-only here. startPersistentAllocationCounting()
    // resets the allocation sampler session, including its completed call tree.
    // Normal profile export is a two-phase stopSampling() -> exportData() flow, so
    // the exporter resumes persistent counting immediately after serialization.
    return true;
}

bool Profiler::resumePersistentAllocationCounting(std::string &error)
{
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    error.clear();
    // A full allocation profile itself accounts allocation bytes. Calling resume
    // while it is still running is a pure no-op; never create a second sampler.
    if (running_.load(std::memory_order_acquire) && mode_ == ProfileMode::Allocation) {
        return true;
    }
    // A count-only backend can be stopped independently of the foreground profile
    // mode. Reap it before trusting the active flag or resetting its counters.
    if (persistent_allocation_counting_active_.load(std::memory_order_acquire) && !allocation_sampler_.running()) {
        if (!stopPersistentAllocationCounting(error)) {
            return false;
        }
    }
    // Retire native ownership before discarding even a failed session.
    if (mode_ == ProfileMode::Allocation && !running_.load(std::memory_order_acquire) &&
        !persistent_allocation_counting_active_.load(std::memory_order_acquire)) {
        const auto cleanup_pending = [this] {
            return allocation_sampler_.running() || allocation_sampler_.backendCleanupPending() ||
                   allocation_sampler_.aggregatorMayBeAlive();
        };
        if (cleanup_pending()) {
            std::string cleanup_error;
            allocation_sampler_.stop(cleanup_error);
            if (cleanup_pending()) {
                error = cleanup_error.empty() ? "the allocation backend is still finishing cleanup"
                                              : std::move(cleanup_error);
                return false;
            }
        }
        if (persistent_allocation_counting_enabled_.load(std::memory_order_acquire) &&
            !persistent_allocation_stop_accumulated_.exchange(true, std::memory_order_acq_rel)) {
            accumulatePersistentAllocationBytes();
        }
        stopRecoveryWriter();
    }
    // Calling resume is the hand-off that says a completed allocation tree has
    // already been serialized or intentionally discarded. Only after native
    // cleanup succeeds may count-only reset/reuse the allocation sampler.
    allocation_export_pending_.store(false, std::memory_order_release);
    if (!persistent_allocation_counting_enabled_.load(std::memory_order_acquire) ||
        persistent_allocation_counting_active_.load(std::memory_order_acquire)) {
        return true;
    }
    return startPersistentAllocationCounting(error);
}

void Profiler::stopSampling()
{
    std::string ignored;
    stopSampling(ignored);
}

std::string Profiler::stop(const ExportContext &ctx)
{
    if (!running_.load()) {
        return {};
    }
    std::string error;
    if (!stopSampling(error)) {
        std::string ignored;
        resumePersistentAllocationCounting(ignored);
        return {};
    }
    try {
        std::string data = exportData(ctx);
        std::string ignored;
        resumePersistentAllocationCounting(ignored);
        return data;
    }
    catch (...) {
        std::string ignored;
        resumePersistentAllocationCounting(ignored);
        throw;
    }
}

std::string Profiler::liveExport(const ExportContext &ctx)
{
    CiDiagnostics::LiveExportScope live_export_scope;
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (!running_.load() || sampling_stop_requested_.load(std::memory_order_acquire)) {
        return {};
    }
    if (mode_ == ProfileMode::Allocation) {
        const bool tracking_was_suppressed = allocation_sampler_.setCurrentThreadTrackingSuppressed(true);
        try {
            AllocationSnapshot snapshot;
            std::string snapshot_error;
            if (!allocation_sampler_.snapshot(snapshot, snapshot_error)) {
                allocation_sampler_.setCurrentThreadTrackingSuppressed(tracking_was_suppressed);
                if (!snapshot_error.empty()) {
                    throw std::runtime_error(snapshot_error);
                }
                return {};
            }
            if (live_export_paused_hook_) {
                live_export_paused_hook_();
            }
            std::string data = exportData(ctx, &snapshot);
            allocation_sampler_.setCurrentThreadTrackingSuppressed(tracking_was_suppressed);
            return data;
        }
        catch (...) {
            allocation_sampler_.setCurrentThreadTrackingSuppressed(tracking_was_suppressed);
            throw;
        }
    }
    sampler_.pauseForExport();
    std::string sampler_error;
    if (sampler_.failure(sampler_error)) {
        if (!sampler_.stop()) {
            throw std::runtime_error(sampler_.lastError());
        }
        running_.store(false);
        stopRecoveryWriter();
        throw std::runtime_error(sampler_error);
    }
    std::string data;
    try {
        if (live_export_paused_hook_) {
            live_export_paused_hook_();
        }
        data = exportData(ctx);
    }
    catch (...) {
        if (sampling_stop_requested_.load(std::memory_order_acquire)) {
            sampler_.stop();
        }
        else if (!sampler_.resumeAfterExport()) {
            sampler_.stop();
            running_.store(false);
            stopRecoveryWriter();
        }
        throw;
    }
    if (sampling_stop_requested_.load(std::memory_order_acquire)) {
        if (!sampler_.stop()) {
            throw std::runtime_error(sampler_.lastError());
        }
        return data;
    }
    if (!sampler_.resumeAfterExport()) {
        sampler_.stop();
        running_.store(false);
        stopRecoveryWriter();
        throw std::runtime_error("the sampler service threads could not be resumed after live export");
    }
    return data;
}

bool Profiler::cancel(std::string &error)
{
    std::string stop_error;
    if (stopSampling(stop_error)) {
        std::string resume_error;
        if (!resumePersistentAllocationCounting(resume_error)) {
            error = resume_error.empty() ? "the allocation backend cleanup is incomplete" : std::move(resume_error);
            return false;
        }
        const RecoveryDiscardResult discard_result = discardRecoveryJournal();
        if (!discard_result.completed()) {
            error =
                discard_result.message.empty() ? "recovery journal cleanup did not complete" : discard_result.message;
            return false;
        }
        error.clear();
        return true;
    }

    // A backend failure invalidates the data; if stopSampling completed native cleanup,
    // cancel is still successful. Resume persistent count-only because there will be no
    // export phase to do it for us.
    if (!running_.load()) {
        std::string resume_error;
        if (!resumePersistentAllocationCounting(resume_error)) {
            error = resume_error.empty() ? "the allocation backend cleanup is incomplete" : std::move(resume_error);
            return false;
        }
        error.clear();
        const RecoveryDiscardResult discard_result = discardRecoveryJournal();
        if (!discard_result.completed()) {
            error =
                discard_result.message.empty() ? "recovery journal cleanup did not complete" : discard_result.message;
            return false;
        }
        return true;
    }

    error = std::move(stop_error);
    return false;
}

void Profiler::cancel()
{
    std::string ignored;
    cancel(ignored);
}

bool Profiler::shutdown(std::string &error)
{
    CiDiagnostics *diagnostics = globalCiDiagnostics();
    CiDiagnostics::Scope diagnostic_scope(
        diagnostics, CiDiagnosticContext::Profiler, CiDiagnosticPhase::ProfilerShutdownEnter,
        CiDiagnosticPhase::ProfilerShutdownExit, CiDiagnosticPhase::ProfilerShutdownExceptionalExit,
        ciDiagnosticCurrentThreadId());
    sampling_stop_requested_.store(true, std::memory_order_release);
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    error.clear();
    // requestStop() may have cleared running_ while service threads still exist.
    // Stop both native backends explicitly before releasing their sinks.
    if (!sampler_.stop()) {
        error = sampler_.lastError();
        return false;
    }
    const bool allocation_backend_active = allocation_sampler_.running() || allocation_sampler_.backendCleanupPending();
    const bool persistent_counting_active = persistent_allocation_counting_active_.load(std::memory_order_acquire);
    if (!allocation_sampler_.shutdown(error)) {
        return false;
    }

    if (persistent_allocation_counting_enabled_.load(std::memory_order_acquire) &&
        (persistent_counting_active || allocation_backend_active) &&
        !persistent_allocation_stop_accumulated_.exchange(true, std::memory_order_acq_rel)) {
        accumulatePersistentAllocationBytes();
    }
    persistent_allocation_counting_active_.store(false, std::memory_order_release);
    persistent_allocation_counting_enabled_.store(false, std::memory_order_release);
    persistent_allocation_session_seed_ = 0;
    running_.store(false, std::memory_order_release);

    // Native backends must be quiescent before the recovery writer is released.
    stopRecoveryWriter();
    if (hasPendingRecoveryWriter()) {
        error = "recovery writer shutdown timed out";
        return false;
    }
    if (retain_recovery_journal_on_shutdown_.load(std::memory_order_acquire)) {
        return true;
    }
    const RecoveryDiscardResult discard_result = discardRecoveryJournal();
    if (!discard_result.completed()) {
        error = discard_result.message;
        if (error.empty()) {
            error = "recovery journal cleanup did not complete";
        }
        return false;
    }
    allocation_export_pending_.store(false, std::memory_order_release);
    return true;
}

}  // namespace spark
