#include <cstdio>
#include <limits>
#include <utility>

#include "core/profiler/profiler.h"

namespace spark {

void Profiler::stopRecoveryWriter()
{
    // A pending backend may still journal: keep the writer and both sinks intact.
    if (allocation_sampler_.backendCleanupPending()) {
        return;
    }
    RecoveryWriter *writer = nullptr;
    {
        std::scoped_lock lock(recovery_mutex_);
        sampler_.setRecoverySink(nullptr);
        allocation_sampler_.setRecoverySink(nullptr);
        writer = recovery_writer_.get();
    }
    if (!writer) {
        return;
    }

    // Do NOT journal CleanEnd here. CleanEnd before export would make a crash during
    // export look clean on the next startup. The journal is deleted only after a
    // successful export, cancel, or clean shutdown.
    writer->requestFlush();
    if (!writer->stop()) {
        // Keep ownership: the worker may still be executing Spark code or file I/O.
        return;
    }

    std::scoped_lock lock(recovery_mutex_);
    if (recovery_writer_.get() == writer) {
        captureRetiredJournalDegradationLocked(*writer);
        recovery_writer_.reset();
        journal_degradation_active_.store(false, std::memory_order_release);
    }
}

bool Profiler::reapRecoveryWriter()
{
    if (allocation_sampler_.backendCleanupPending()) {
        return false;
    }
    std::scoped_lock lock(recovery_mutex_);
    if (!recovery_writer_) {
        return true;
    }
    if (!recovery_writer_->tryReap()) {
        return false;
    }
    captureRetiredJournalDegradationLocked(*recovery_writer_);
    recovery_writer_.reset();
    journal_degradation_active_.store(false, std::memory_order_release);
    return true;
}

bool Profiler::hasPendingRecoveryWriter() const
{
    std::scoped_lock lock(recovery_mutex_);
    return recovery_writer_ != nullptr;
}

void Profiler::recordJournalDegradationOverflowLocked() noexcept
{
    if (pending_journal_degradation_overflow_count_ != std::numeric_limits<std::uint32_t>::max()) {
        ++pending_journal_degradation_overflow_count_;
    }
    pending_journal_degradation_notice_.store(true, std::memory_order_release);
}

void Profiler::captureRetiredJournalDegradationLocked(RecoveryWriter &writer) noexcept
{
    if (!writer.journalDegraded() || journal_degradation_logged_.load(std::memory_order_acquire)) {
        return;
    }
    if (pending_journal_degradation_count_ == kPendingJournalDegradationCapacity) {
        recordJournalDegradationOverflowLocked();
        return;
    }

    try {
        std::string cause = writer.journalDegradationReason();
        if (cause.empty()) {
            cause = "recovery journal degraded";
        }

        const auto tail = (pending_journal_degradation_head_ + pending_journal_degradation_count_) %
                          kPendingJournalDegradationCapacity;
        pending_journal_degradation_causes_[tail] = std::move(cause);
        ++pending_journal_degradation_count_;
        pending_journal_degradation_notice_.store(true, std::memory_order_release);
    }
    catch (...) {
        recordJournalDegradationOverflowLocked();
        std::fputs("Spark recovery journal degradation notice was coalesced.\n", stderr);
    }
}

RecoveryDiscardResult Profiler::discardRecoveryJournal()
{
    if (allocation_sampler_.backendCleanupPending()) {
        retain_recovery_journal_on_shutdown_.store(true, std::memory_order_release);
        return {.status = RecoveryDiscardStatus::BackendCleanupPending,
                .message = "allocation backend cleanup is still pending"};
    }
    stopRecoveryWriter();
    if (hasPendingRecoveryWriter()) {
        retain_recovery_journal_on_shutdown_.store(true, std::memory_order_release);
        return {.status = RecoveryDiscardStatus::WriterPending, .message = "recovery writer shutdown timed out"};
    }
    if (recovery_dir_.empty()) {
        retain_recovery_journal_on_shutdown_.store(false, std::memory_order_release);
        return {};
    }

    std::error_code ec;
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
    if (recovery_remove_function_) {
        recovery_remove_function_(recovery_dir_, ec);
    }
    else {
        std::filesystem::remove_all(recovery_dir_, ec);
    }
#else
    std::filesystem::remove_all(recovery_dir_, ec);
#endif
    if (ec) {
        retain_recovery_journal_on_shutdown_.store(true, std::memory_order_release);
        return {.status = RecoveryDiscardStatus::FilesystemError,
                .message = "recovery journal cleanup failed: " + ec.message()};
    }
    retain_recovery_journal_on_shutdown_.store(false, std::memory_order_release);
    return {};
}

void Profiler::journalStallBegin(std::uint64_t detected_ns, std::uint64_t last_tick_ns)
{
    std::scoped_lock lock(recovery_mutex_);
    if (recovery_writer_) {
        recovery_writer_->journalStallBegin(detected_ns, last_tick_ns);
        recovery_writer_->requestFlush();
    }
}

void Profiler::journalStallEnd(std::uint64_t detected_ns, std::uint64_t recovered_ns)
{
    std::scoped_lock lock(recovery_mutex_);
    if (recovery_writer_) {
        recovery_writer_->journalStallEnd(detected_ns, recovered_ns);
        recovery_writer_->requestFlush();
    }
}

bool Profiler::reportJournalDegradationIfNeeded(std::string &cause)
{
    if (!journal_degradation_active_.load(std::memory_order_relaxed) &&
        !pending_journal_degradation_notice_.load(std::memory_order_acquire)) {
        return false;
    }
    std::scoped_lock lock(recovery_mutex_);
    if (pending_journal_degradation_count_ != 0) {
        cause = pending_journal_degradation_causes_[pending_journal_degradation_head_];
        pending_journal_degradation_causes_[pending_journal_degradation_head_].clear();
        pending_journal_degradation_head_ =
            (pending_journal_degradation_head_ + 1) % kPendingJournalDegradationCapacity;
        --pending_journal_degradation_count_;
    }
    else if (pending_journal_degradation_overflow_count_ != 0) {
        cause = "additional recovery journals degraded";
        pending_journal_degradation_overflow_count_ = 0;
    }
    else if (journal_degradation_active_.load(std::memory_order_relaxed) && recovery_writer_ &&
             recovery_writer_->journalDegraded() && !journal_degradation_logged_.load(std::memory_order_acquire)) {
        cause = recovery_writer_->journalDegradationReason();
        journal_degradation_logged_.store(true, std::memory_order_release);
        return true;
    }
    else {
        return false;
    }

    pending_journal_degradation_notice_.store(pending_journal_degradation_count_ != 0 ||
                                                  pending_journal_degradation_overflow_count_ != 0,
                                              std::memory_order_release);
    return true;
}

}  // namespace spark
