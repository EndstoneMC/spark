#include "application/profiler/profiler_service.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "application/profiler/platform_metadata_capture.h"
#include "core/util/monotonic_time.h"
#include "native/diagnostics/ci_diagnostics.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return monotonicUnixMillis();
}

}  // namespace

ProfilerService::ProfilerService(StatisticsService &statistics, std::string bds_executable_sha256,
                                 std::filesystem::path profile_storage_dir, std::string bytebin_url,
                                 std::string viewer_url, std::string bytesocks_host, bool background_enabled,
                                 int background_interval, std::string background_thread_grouper,
                                 std::string background_thread_dumper, TrustedViewersState &trusted_viewers,
                                 MainThreadDispatcher &dispatcher, ProfileMetadataProvider &metadata_provider,
                                 ResultNotifier &notifier)
    : statistics_(statistics), bds_executable_sha256_(std::move(bds_executable_sha256)), dispatcher_(dispatcher),
      metadata_provider_(metadata_provider), notifier_(notifier),
      exporter_(std::move(profile_storage_dir), bytebin_url, viewer_url), background_enabled_(background_enabled),
      background_interval_(background_interval), background_thread_grouper_(std::move(background_thread_grouper)),
      background_thread_dumper_(std::move(background_thread_dumper)), bytebin_url_(std::move(bytebin_url)),
      viewer_url_(std::move(viewer_url)), bytesocks_host_(std::move(bytesocks_host)), trusted_viewers_(trusted_viewers)
{
    viewer_open_ = std::make_unique<ProfilerOpenOrchestrator>(
        profiler_, statistics_, bds_executable_sha256_, bytebin_url_, viewer_url_, bytesocks_host_, trusted_viewers_,
        dispatcher_, metadata_provider_, notifier_);
    viewer_open_->setNativePluginSourcesProvider([this]() { return session_native_plugin_sources_; });
    viewer_open_->setPingSamplesProvider(
        [this]() { return ping_samples_provider_ ? ping_samples_provider_() : std::vector<int>(); });
    viewer_open_->setNetworkSnapshotProvider([this]() {
        return network_snapshot_provider_ ? network_snapshot_provider_()
                                          : std::map<std::string, NetworkInterfaceSnapshot>();
    });
}

ProfilerService::~ProfilerService()
{
    std::string error;
    if (!shutdown(error)) {
        std::terminate();
    }
}

void ProfilerService::shutdown()
{
    std::string ignored;
    shutdown(ignored);
}

bool ProfilerService::shutdown(std::string &error)
{
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
    const auto deadline = std::chrono::steady_clock::now() + export_shutdown_timeout_;
#else
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
#endif
    error.clear();
    stopping_.store(true, std::memory_order_release);
    requestProfilerTimeoutStop();
    profiler_.requestStop();
    {
        std::scoped_lock lock(export_mutex_);
        export_stop_requested_ = true;
        export_cancellation_.requestStop();
    }
    if (exporting_.load(std::memory_order_acquire)) {
        preserve_recovery_journal_on_shutdown_ = true;
        profiler_.retainRecoveryJournalOnShutdown();
    }
    export_cv_.notify_all();
    if (viewer_open_) {
        viewer_open_->requestStop();
    }

    const bool timer_stopped = profiler_timeout_.reapUntil(deadline);
    const bool export_stopped = waitForExportWorkerUntil(deadline);
    const bool viewer_stopped = !viewer_open_ || viewer_open_->shutdownUntil(deadline);
    if (!timer_stopped || !export_stopped || !viewer_stopped) {
        if (!timer_stopped) {
            error = "profiler timer did not stop within 5000 milliseconds";
        }
        else if (!export_stopped) {
            error = "profile export worker did not stop within 5000 milliseconds";
        }
        else {
            error = "live viewer did not stop within 5000 milliseconds";
        }
        return false;
    }

    // Shutdown has no safe sender for a late result. Consume it only after the
    // worker has stopped, retaining the journal for cancellation/failure.
    consumeFinishedExport(false);
    {
        std::scoped_lock lock(export_mutex_);
        export_job_.reset();
        export_result_.reset();
        export_completion_pending_.store(false, std::memory_order_release);
        export_worker_exited_ = true;
    }
    exporting_.store(false);
    restart_background_after_export_ = false;
    lifetime_.reset();
    return true;
}

ExportContext ProfilerService::captureLiveContext(std::int64_t now_ms)
{
    return viewer_open_->captureLiveContext(now_ms);
}

std::string ProfilerService::buildLiveSamplerData(const ExportContext &context)
{
    return viewer_open_->buildLiveSamplerData(context);
}

void ProfilerService::closeViewerSocket()
{
    if (viewer_open_) {
        viewer_open_->close();
    }
}

void ProfilerService::resetProfilerTimeout() noexcept
{
    static_cast<void>(resetProfilerTimeoutUntil(std::chrono::steady_clock::now()));
}

bool ProfilerService::resetProfilerTimeoutUntil(std::chrono::steady_clock::time_point deadline) noexcept
{
    requestProfilerTimeoutStop();
    return profiler_timeout_.reapUntil(deadline);
}

void ProfilerService::requestProfilerTimeoutStop() noexcept
{
    if (CiDiagnostics *diagnostics = globalCiDiagnostics(); diagnostics != nullptr) {
        diagnostics->publish(CiDiagnosticContext::Timeout, CiDiagnosticPhase::TimeoutCancel);
    }
    timeout_generation_.fetch_add(1, std::memory_order_acq_rel);
    timeout_completion_pending_.store(0, std::memory_order_release);
    profiler_timeout_.requestStop();
}

bool ProfilerService::armProfilerTimeout(std::int64_t timeout_seconds) noexcept
{
    if (stopping_.load(std::memory_order_acquire) || !profiler_timeout_.reapUntil(std::chrono::steady_clock::now())) {
        return false;
    }
    if (timeout_seconds <= 0) {
        timeout_completion_pending_.store(0, std::memory_order_release);
        return true;
    }

    using MillisecondsRep = std::chrono::milliseconds::rep;
    constexpr std::int64_t k_milliseconds_per_second = 1000;
    constexpr auto k_maximum_seconds =
        static_cast<std::int64_t>(std::numeric_limits<MillisecondsRep>::max() / k_milliseconds_per_second);
    if (timeout_seconds > k_maximum_seconds) {
        return false;
    }

    timeout_completion_pending_.store(0, std::memory_order_release);
    const auto generation = timeout_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const auto delay = std::chrono::milliseconds(static_cast<MillisecondsRep>(timeout_seconds) *
                                                 static_cast<MillisecondsRep>(k_milliseconds_per_second));
    if (CiDiagnostics *diagnostics = globalCiDiagnostics(); diagnostics != nullptr) {
        diagnostics->publish(CiDiagnosticContext::Timeout, CiDiagnosticPhase::TimeoutArm);
    }
    return profiler_timeout_.arm(delay, [this, generation]() noexcept {
        if (CiDiagnostics *diagnostics = globalCiDiagnostics(); diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::TimeoutWorker, CiDiagnosticPhase::TimeoutFired,
                                 ciDiagnosticCurrentThreadId());
        }
        if (timeout_generation_.load(std::memory_order_acquire) != generation ||
            stopping_.load(std::memory_order_acquire)) {
            return;
        }
        profiler_.requestStop();
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (timeout_publication_hook_) {
            timeout_publication_hook_();
        }
#endif
        timeout_completion_pending_.store(generation, std::memory_order_release);
    });
}

void ProfilerService::finishProfiler(const std::string &sender_name, bool sender_is_player,
                                     std::string sender_unique_id, bool save, const std::string &comment)
{
    const auto notify_best_effort = [this](const std::string &name, const std::string &message) noexcept {
        try {
            [&] {
                CiDiagnostics::Scope diagnostic_scope(
                    globalCiDiagnostics(), CiDiagnosticContext::Notification, CiDiagnosticPhase::NotificationEnter,
                    CiDiagnosticPhase::NotificationExit, CiDiagnosticPhase::NotificationExceptionalExit);
                notifier_.notify(name, message);
            }();
        }
        catch (...) {  // NOLINT(bugprone-empty-catch): notification is best effort.
        }
    };
    const auto restore_background = [this]() noexcept {
        background_started_ = false;
        if (!restart_background_after_export_) {
            return;
        }
        restart_background_after_export_ = false;
        background_suppressed_ = false;
        background_started_ = startBackgroundSession();
    };

    resetProfilerTimeout();
    std::string stop_error;
    if (!profiler_.stopSampling(stop_error)) {
        const bool stopped = !profiler_.running();
        std::string backend_error;
        const bool backend_failed = stopped && profiler_.backendFailure(backend_error);
        if (stopped) {
            session_type_ = SessionType::None;
            std::string resume_error;
            profiler_.resumePersistentAllocationCounting(resume_error);
            restore_background();
        }
        if (backend_failed) {
            notify_best_effort(sender_name,
                               "Allocation profiler FAILED; incomplete profile data was discarded: " + backend_error);
            notify_best_effort(sender_name, "The allocation profiler backend is ready for a new session.");
        }
        else {
            notify_best_effort(sender_name, "Profiler stop failed: " + stop_error);
        }
        return;
    }
    session_type_ = SessionType::None;

    ExportContext context;
    try {
        context.bds_executable_sha256 = bds_executable_sha256_;
        gatherPlatformServerMetadata(metadata_provider_, context, nowMs());
        context.native_plugin_sources = session_native_plugin_sources_;
        context.comment = comment;
        context.statistics = statistics_.snapshot();
        context.metrics = statistics_.metricsSnapshot();
        context.window_stats = statistics_.profileWindows(profiler_.startTimeMs(), profiler_.endTimeMs());
        context.system_stats = spark::gatherSystemStats(".");
        metadata_provider_.gatherWorldMetadata(context.world, context.minecraft_version);
        if (ping_samples_provider_) {
            context.ping_samples = ping_samples_provider_();
        }
        if (network_snapshot_provider_) {
            context.net_snapshots = network_snapshot_provider_();
        }
    }
    catch (const std::exception &error) {
        std::string resume_error;
        profiler_.resumePersistentAllocationCounting(resume_error);
        restore_background();
        try {
            notify_best_effort(sender_name, std::string("Failed to prepare the profile export: ") + error.what());
        }
        catch (...) {  // NOLINT(bugprone-empty-catch): notification is best effort.
        }
        return;
    }
    catch (...) {
        std::string resume_error;
        profiler_.resumePersistentAllocationCounting(resume_error);
        restore_background();
        notify_best_effort(sender_name, "Failed to prepare the profile export.");
        return;
    }

    try {
        ensureExportWorker();

        ExportJob job;
        job.context = std::move(context);
        job.save_to_file = save;
        job.sender = sender_name;
        job.sender_is_player = sender_is_player;
        job.sender_unique_id = sender_is_player ? std::move(sender_unique_id) : std::string{};
        job.lifetime_token = lifetime_;
        {
            std::scoped_lock lock(export_mutex_);
            if (export_stop_requested_ || export_job_.has_value() || export_result_.has_value()) {
                throw std::runtime_error("the profile export worker is not ready for a new job");
            }
            export_cancellation_.reset();
            job.cancellation = export_cancellation_.token();
            export_job_ = std::move(job);
            exporting_.store(true, std::memory_order_release);
        }
        export_cv_.notify_one();
    }
    catch (const std::exception &error) {
        exporting_.store(false);
        std::string resume_error;
        profiler_.resumePersistentAllocationCounting(resume_error);
        restore_background();
        notify_best_effort(sender_name, std::string("Failed to start the profile export worker: ") + error.what());
    }
    catch (...) {
        exporting_.store(false);
        std::string resume_error;
        profiler_.resumePersistentAllocationCounting(resume_error);
        restore_background();
        notify_best_effort(sender_name, "Failed to start the profile export worker.");
    }
}

void ProfilerService::ensureExportWorker()
{
    std::unique_lock lock(export_mutex_);
    if (export_thread_.joinable()) {
        if (export_worker_exited_) {
            throw std::runtime_error("the profile export worker has exited");
        }
        return;
    }
    if (export_stop_requested_) {
        throw std::runtime_error("the profile export worker is stopping");
    }
    export_worker_exited_ = false;
    lock.unlock();
    try {
        if (!export_thread_.start([this] { exportWorkerLoop(); })) {
            throw std::runtime_error("the profile export worker could not start");
        }
    }
    catch (...) {
        lock.lock();
        export_worker_exited_ = true;
        throw;
    }
}

void ProfilerService::exportWorkerLoop() noexcept
{
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
    {
        std::scoped_lock lock(export_mutex_);
        export_worker_id_for_testing_ = std::this_thread::get_id();
    }
#endif
    CiDiagnostics *diagnostics = globalCiDiagnostics();
    const std::uint64_t worker_tid = ciDiagnosticCurrentThreadId();
    std::optional<ExportJob> active_job;
    try {
        for (;;) {
            {
                std::unique_lock lock(export_mutex_);
                export_cv_.wait(lock, [this] { return export_stop_requested_ || export_job_.has_value(); });
                if (!export_job_.has_value()) {
                    break;
                }
                active_job = std::move(*export_job_);
                export_job_.reset();
            }

#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
            if (export_job_preparation_hook_) {
                export_job_preparation_hook_();
            }
#endif
            ExportResult published;
            published.sender = std::move(active_job->sender);
            published.sender_is_player = active_job->sender_is_player;
            published.sender_unique_id = std::move(active_job->sender_unique_id);
            try {
                CiDiagnostics::Scope diagnostic_scope(diagnostics, CiDiagnosticContext::Export,
                                                      CiDiagnosticPhase::ExportEnter, CiDiagnosticPhase::ExportComplete,
                                                      CiDiagnosticPhase::ExportFailed, worker_tid);
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
                ProfileExporter::Result result;
                if (export_function_) {
                    result = export_function_(profiler_, active_job->context, active_job->save_to_file,
                                              active_job->cancellation);
                }
                else {
                    result = exporter_.exportProfile(profiler_, std::move(active_job->context),
                                                     active_job->save_to_file, active_job->cancellation);
                }
#else
                ProfileExporter::Result result = exporter_.exportProfile(
                    profiler_, std::move(active_job->context), active_job->save_to_file, active_job->cancellation);
#endif
                published.outcome = result.outcome;
                published.message = std::move(result.message);
                published.retain_recovery_journal = result.retain_recovery_journal;
            }
            catch (const std::exception &error) {
                std::string ignored;
                profiler_.resumePersistentAllocationCounting(ignored);
                published.message = std::string("Export failed: ") + error.what();
                published.retain_recovery_journal = active_job->cancellation.stopRequested();
            }
            catch (...) {
                std::string ignored;
                profiler_.resumePersistentAllocationCounting(ignored);
                published.message = "Export failed with an unknown error.";
                published.retain_recovery_journal = active_job->cancellation.stopRequested();
            }

            {
                std::scoped_lock lock(export_mutex_);
                export_result_ = std::move(published);
                export_completion_pending_.store(true, std::memory_order_release);
            }
            active_job.reset();
            export_cv_.notify_all();
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
            if (export_post_publication_hook_) {
                export_post_publication_hook_();
            }
#endif
        }
    }
    catch (...) {
        // The worker is only allowed to end through the shutdown state. A
        // current job still receives a terminal result before that state is published.
        std::optional<ExportJob> abandoned;
        {
            std::scoped_lock lock(export_mutex_);
            if (active_job.has_value()) {
                abandoned = std::move(active_job);
                active_job.reset();
            }
            else if (export_job_.has_value()) {
                abandoned = std::move(export_job_);
                export_job_.reset();
            }
            if (!export_result_.has_value() && abandoned.has_value()) {
                ExportResult published;
                published.sender = abandoned->sender;
                published.sender_is_player = abandoned->sender_is_player;
                published.sender_unique_id = std::move(abandoned->sender_unique_id);
                published.message = "Export failed with an unknown error.";
                published.retain_recovery_journal = abandoned->cancellation.stopRequested();
                export_result_ = std::move(published);
                export_completion_pending_.store(true, std::memory_order_release);
            }
        }
        export_cv_.notify_all();
    }

    // No local job or I/O remains after the exited flag is published.
    {
        std::scoped_lock lock(export_mutex_);
        export_worker_exited_ = true;
    }
    export_exit_cv_.notify_all();
}

bool ProfilerService::waitForExportWorker(std::chrono::milliseconds timeout)
{
    return waitForExportWorkerUntil(std::chrono::steady_clock::now() + timeout);
}

bool ProfilerService::waitForExportWorkerUntil(std::chrono::steady_clock::time_point deadline)
{
    return export_thread_.reapUntil(deadline);
}

bool ProfilerService::consumeFinishedExport(bool notify) noexcept
{
    std::optional<ExportResult> result;
    {
        std::scoped_lock lock(export_mutex_);
        if (!export_result_.has_value()) {
            return false;
        }
        result = std::move(export_result_);
        export_result_.reset();
        export_completion_pending_.store(false, std::memory_order_release);
    }
    exporting_.store(false, std::memory_order_release);
    if (notify) {
        announceResult(std::move(*result));
    }
    else {
        if (preserve_recovery_journal_on_shutdown_ ||
            (result->outcome != ExportOutcome::Uploaded && result->outcome != ExportOutcome::Saved) ||
            result->retain_recovery_journal) {
            profiler_.retainRecoveryJournalOnShutdown();
        }
        else {
            try {
                const RecoveryDiscardResult discard_result = profiler_.discardRecoveryJournal();
                if (!discard_result.completed()) {
                    profiler_.retainRecoveryJournalOnShutdown();
                }
            }
            catch (...) {  // NOLINT(bugprone-empty-catch): shutdown has no notifier.
                profiler_.retainRecoveryJournalOnShutdown();
            }
        }
    }
    return true;
}

void ProfilerService::announceResult() noexcept
{
    ExportResult result;
    result.outcome = pending_outcome_;
    result.sender = std::move(pending_sender_);
    result.sender_is_player = pending_sender_is_player_;
    result.sender_unique_id = std::move(pending_sender_unique_id_);
    result.message = std::move(pending_result_);
    announceResult(std::move(result));
}

void ProfilerService::announceResult(ExportResult result) noexcept
{
    CiDiagnostics::Scope diagnostic_scope(globalCiDiagnostics(), CiDiagnosticContext::Completion,
                                          CiDiagnosticPhase::CompletionEnter, CiDiagnosticPhase::CompletionExit,
                                          CiDiagnosticPhase::CompletionExceptionalExit, ciDiagnosticCurrentThreadId());
    const ExportOutcome outcome = result.outcome;
    const std::string sender = std::move(result.sender);
    const bool sender_is_player = result.sender_is_player;
    const std::string sender_unique_id = std::move(result.sender_unique_id);
    std::string result_message = std::move(result.message);
    const char *headline = "Profiler stopped.";
    if (outcome == ExportOutcome::Uploaded) {
        headline = "Profiler stopped & upload complete!";
    }
    else if (outcome == ExportOutcome::Saved) {
        headline = "Profiler stopped & saved locally!";
    }

    if ((outcome != ExportOutcome::Uploaded && outcome != ExportOutcome::Saved) || result.retain_recovery_journal) {
        profiler_.retainRecoveryJournalOnShutdown();
    }

    // A successful export means the profile is safely delivered; journal cleanup
    // is a separate best-effort operation and never changes that outcome.
    if (outcome == ExportOutcome::Uploaded || outcome == ExportOutcome::Saved) {
        if (result.retain_recovery_journal) {
            result_message += " Recovery journal retained: shutdown was requested.";
        }
        else {
            try {
                const RecoveryDiscardResult discard_result = profiler_.discardRecoveryJournal();
                if (!discard_result.completed()) {
                    profiler_.retainRecoveryJournalOnShutdown();
                    result_message += " Recovery journal retained: " + discard_result.message;
                }
            }
            catch (...) {  // NOLINT(bugprone-empty-catch): a delivered profile remains usable.
                profiler_.retainRecoveryJournalOnShutdown();
                result_message += " Recovery journal retained: cleanup failed.";
            }
        }
    }

    exporting_.store(false, std::memory_order_release);
    if (restart_background_after_export_) {
        restart_background_after_export_ = false;
        background_suppressed_ = false;
        background_started_ = startBackgroundSession();
    }

    const auto notify_best_effort = [this](const std::string &name, const std::string &message) noexcept {
        try {
            [&] {
                CiDiagnostics::Scope diagnostic_scope(
                    globalCiDiagnostics(), CiDiagnosticContext::Notification, CiDiagnosticPhase::NotificationEnter,
                    CiDiagnosticPhase::NotificationExit, CiDiagnosticPhase::NotificationExceptionalExit);
                notifier_.notify(name, message);
            }();
        }
        catch (...) {  // NOLINT(bugprone-empty-catch): completion notification is best effort.
        }
    };
    notify_best_effort(sender, headline);
    notify_best_effort(sender, result_message);

    try {
        if (activity_log_provider_) {
            ActivityLog *log = activity_log_provider_();
            if (log) {
                const std::int64_t now_ms = nowMs();
                if (outcome == ExportOutcome::Uploaded) {
                    log->add(
                        Activity::url(sender, sender_is_player, now_ms, "Profiler", result_message, sender_unique_id));
                }
                else if (outcome == ExportOutcome::Saved) {
                    log->add(
                        Activity::file(sender, sender_is_player, now_ms, "Profiler", result_message, sender_unique_id));
                }
            }
        }
    }
    catch (...) {  // NOLINT(bugprone-empty-catch): activity logging is best effort.
    }
}

void ProfilerService::onTick(double mspt)
{
    profiler_timeout_.reapUntil(std::chrono::steady_clock::now());
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }
    consumeFinishedExport(true);
    const auto completed_generation = timeout_completion_pending_.exchange(0, std::memory_order_acq_rel);
    if (completed_generation != 0 && completed_generation == timeout_generation_.load(std::memory_order_acquire)) {
        if (CiDiagnostics *diagnostics = globalCiDiagnostics(); diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Timeout, CiDiagnosticPhase::TimeoutCompletion,
                                 ciDiagnosticCurrentThreadId());
        }
        resetProfilerTimeout();
        if (profiler_.running()) {
            const bool save = profiler_.options().save_to_file;
            closeViewerSocket();
            finishProfiler(start_sender_name_, start_sender_is_player_, start_sender_unique_id_, save, std::string());
        }
    }
    if (viewer_open_) {
        viewer_open_->onTick(start_sender_name_);
    }
    if (!background_started_ && !background_suppressed_ && background_enabled_ && main_tid_ != 0 &&
        !profiler_.running() && !exporting_.load()) {
        auto now = nowMs();
        if (now >= next_background_retry_ms_) {
            if (startBackgroundSession()) {
                background_started_ = true;
                background_retry_delay_s_ = 0;
            }
            else {
                // Exponential backoff: 5s -> 15s -> 30s -> 60s (cap).
                if (background_retry_delay_s_ == 0) {
                    background_retry_delay_s_ = 5;
                }
                else if (background_retry_delay_s_ < 60) {
                    background_retry_delay_s_ = std::min(60, background_retry_delay_s_ * 2);
                }
                next_background_retry_ms_ = now + background_retry_delay_s_ * 1000;
            }
        }
    }

    // Persistent allocation-rate counting is intentionally active even when no
    // full profiler session is running. Tick it in that idle state so a transient
    // post-export resume failure can be retried without waiting for another full
    // profile, while allocation_export_pending_ inside Profiler still prevents an
    // early reset before completed SamplerData serialization/discard.
    if (!profiler_.running()) {
        profiler_.onTick(mspt);
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
        closeViewerSocket();
        finishProfiler(start_sender_name_, start_sender_is_player_, start_sender_unique_id_, save, std::string());
    }
}

void ProfilerService::startBackgroundProfiler()
{
    background_suppressed_ = false;
    if (!background_enabled_) {
        return;
    }
    if (profiler_.running() || exporting_.load()) {
        return;
    }
    if (startBackgroundSession()) {
        background_started_ = true;
    }
    // If main_tid_ is 0, the background profiler will start on the first tick.
}

bool ProfilerService::startBackgroundSession() noexcept
{
    try {
        if (stopping_.load(std::memory_order_acquire) || !background_enabled_ || profiler_.running() ||
            exporting_.load() || main_tid_ == 0) {
            return false;
        }

        const auto replacement_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        const bool timer_stopped = resetProfilerTimeoutUntil(replacement_deadline);
        const bool viewer_stopped = !viewer_open_ || viewer_open_->retireUntil(replacement_deadline);
        if (!timer_stopped || !viewer_stopped) {
            return false;
        }

        resetProfilerTimeout();

        spark::ProfilerOptions options;
        options.is_background = true;
        options.interval_ms = background_interval_;
        options.timeout_seconds = -1;
        options.ignore_sleeping = false;

        if (background_thread_dumper_ == "all") {
            options.threads = {"*"};
        }

        if (background_thread_grouper_ == "by-name") {
            options.thread_grouper = spark::ThreadGrouperMode::ByName;
        }
        else if (background_thread_grouper_ == "as-one") {
            options.thread_grouper = spark::ThreadGrouperMode::AsOne;
        }
        else {
            options.thread_grouper = spark::ThreadGrouperMode::ByPool;
        }

        std::vector<NativePluginSource> native_plugin_sources = metadata_provider_.nativePluginSources();
        std::string error;
        if (!profiler_.start(options, main_tid_, error)) {
            return false;
        }
        session_native_plugin_sources_ = std::move(native_plugin_sources);

        session_type_ = SessionType::Background;
        return true;
    }
    catch (...) {
        if (profiler_.running()) {
            try {
                profiler_.cancel();
            }
            catch (...) {  // NOLINT(bugprone-empty-catch): startup cleanup is best effort.
            }
        }
        session_type_ = SessionType::None;
        return false;
    }
}

}  // namespace spark
