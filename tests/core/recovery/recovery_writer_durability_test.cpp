#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>

#include <sys/wait.h>
#endif

#include "core/recovery/journal_reader.h"
#include "core/recovery/recovery_writer.h"
#include "core/recovery/replay_guard.h"

namespace spark {

struct RecoveryWriterQueueTestAccess {
    static bool dirty(const RecoveryWriter &writer) { return writer.dirty_; }
    static auto lastSync(const RecoveryWriter &writer) { return writer.last_sync_; }
    static std::size_t totalBytes(const RecoveryWriter &writer) { return writer.total_bytes_; }
    static std::size_t queued(const RecoveryWriter &writer)
    {
        return writer.queue_size_.load(std::memory_order_acquire);
    }
    static bool snapshot(RecoveryWriter &writer) { return writer.writeMetadataSnapshot(); }
    static void reportDegradation(RecoveryWriter &writer, std::string_view cause)
    {
        writer.reportJournalDegradation(cause);
    }
    static void reserve(RecoveryWriter &writer)
    {
        writer.active_producers_.fetch_add(1);
        writer.queue_size_.fetch_add(1);
    }
    static void publishReservedTick(RecoveryWriter &writer, std::uint64_t tick)
    {
        const auto sequence = writer.sequence_.fetch_add(1);
        assert(
            writer.queue_.enqueue(serializeRecord(RecordType::TickEvent, sequence, buildTickEventPayload(tick, 5.0))));
        writer.producerDone();
    }
    static void cancelReservation(RecoveryWriter &writer)
    {
        writer.queue_size_.fetch_sub(1);
        writer.producerDone();
    }
};

}  // namespace spark

namespace {

using namespace std::chrono_literals;
using Operation = spark::RecoveryWriter::IoOperation;

class Gate {
public:
    void enter()
    {
        std::unique_lock lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [&] { return released_; });
    }
    void waitEntered()
    {
        std::unique_lock lock(mutex_);
        assert(cv_.wait_for(lock, 3s, [&] { return entered_; }));
    }
    void release()
    {
        std::scoped_lock lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool released_ = false;
};

class SnapshotAttemptGate {
public:
    using Clock = std::chrono::steady_clock;

    struct Attempt {
        Clock::time_point entered;
        Clock::time_point released;
        int failures = 0;
        int snapshot_attempts = 0;
        std::uint64_t written = 0;
    };

    void enter(int failures, int snapshot_attempts, std::uint64_t written)
    {
        std::unique_lock lock(mutex_);
        if (entered_ >= attempts_.size()) {
            unexpected_entry_ = true;
            cv_.notify_all();
            return;
        }
        const auto index = entered_++;
        attempts_[index] = {.entered = Clock::now(),
                            .released = {},
                            .failures = failures,
                            .snapshot_attempts = snapshot_attempts,
                            .written = written};
        cv_.notify_all();
        if (!cv_.wait_for(lock, 3s, [&] { return released_ > index; })) {
            timed_out_ = true;
        }
        attempts_[index].released = Clock::now();
        completed_ = std::max(completed_, index + 1);
        cv_.notify_all();
    }

    bool waitEntered(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 3s, [&] { return entered_ >= count; });
    }

    bool waitCompleted(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 3s, [&] { return completed_ >= count; });
    }

    Attempt attempt(std::size_t index)
    {
        std::scoped_lock lock(mutex_);
        assert(index < attempts_.size());
        return attempts_[index];
    }

    void releaseThrough(std::size_t count)
    {
        std::scoped_lock lock(mutex_);
        released_ = std::max(released_, std::min(count, attempts_.size()));
        cv_.notify_all();
    }

    void releaseAll() { releaseThrough(attempts_.size()); }

    std::size_t enteredCount()
    {
        std::scoped_lock lock(mutex_);
        return entered_;
    }

    std::size_t completedCount()
    {
        std::scoped_lock lock(mutex_);
        return completed_;
    }

    bool timedOut()
    {
        std::scoped_lock lock(mutex_);
        return timed_out_;
    }

    bool unexpectedEntry()
    {
        std::scoped_lock lock(mutex_);
        return unexpected_entry_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::array<Attempt, 4> attempts_{};
    std::size_t entered_ = 0;
    std::size_t completed_ = 0;
    std::size_t released_ = 0;
    bool timed_out_ = false;
    bool unexpected_entry_ = false;
};

thread_local int DegradationReporter = 0;

template <typename Predicate>
void waitFor(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    assert(predicate());
}

std::filesystem::path testDirectory(const std::string &name)
{
    const auto directory = std::filesystem::temp_directory_path() / "spark_writer_durability" / name;
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

spark::RecoveryWriter::Config configFor(const std::string &name)
{
    spark::RecoveryWriter::Config config;
    config.directory = testDirectory(name);
    config.session_id = 1000;
    config.flush_interval_ms = 10;
    config.sync_interval_ms = 60000;
    return config;
}

void assertRecords(const std::filesystem::path &directory, std::uint64_t count)
{
    const auto journal = spark::JournalReader::readSession(directory);
    assert(journal.valid);
    assert(!journal.fatal_error);
    assert(journal.record_count == count);
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t tick = 0;
        double mspt = 0;
        assert(journal.records.at(static_cast<std::size_t>(i)).asTickEvent(tick, mspt));
        assert(tick == i + 1);
        assert(mspt == 5.0);
    }
}

std::uintmax_t regularFileBytes(const std::filesystem::path &directory)
{
    std::uintmax_t bytes = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        bytes += it->file_size(ec);
        if (ec) {
            return std::numeric_limits<std::uintmax_t>::max();
        }
    }
    return ec ? std::numeric_limits<std::uintmax_t>::max() : bytes;
}

std::uintmax_t segmentFileBytes(const std::filesystem::path &directory)
{
    std::uintmax_t bytes = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".jnl" || !it->is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        bytes += it->file_size(ec);
        if (ec) {
            return std::numeric_limits<std::uintmax_t>::max();
        }
    }
    return ec ? std::numeric_limits<std::uintmax_t>::max() : bytes;
}

void testCompletedOrdering()
{
    auto config = configFor("ordering");
    std::vector<Operation> completed;
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::WriteComplete || operation == Operation::FlushComplete ||
            operation == Operation::SyncComplete || operation == Operation::CloseComplete ||
            operation == Operation::RenameComplete) {
            completed.push_back(operation);
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalTickEvent(1, 5.0);
    assert(writer.stop(3s));
    const std::vector<Operation> expected{
        Operation::WriteComplete, Operation::FlushComplete,  Operation::SyncComplete,
        Operation::CloseComplete, Operation::RenameComplete, Operation::WriteComplete,
        Operation::FlushComplete, Operation::SyncComplete,   Operation::CloseComplete};
    assert(completed == expected);
    assertRecords(config.directory, 1);
}

void testRunningFlush(bool explicit_request)
{
    auto config = configFor(explicit_request ? "explicit" : "idle");
    config.flush_interval_ms = explicit_request ? 60000 : 5;
    config.sync_interval_ms = explicit_request ? 60000 : 150;
    std::atomic<int> completed_syncs{0};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::SyncComplete) {
            completed_syncs.fetch_add(1);
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalTickEvent(1, 5.0);
    if (explicit_request) {
        writer.requestFlush();
    }
    waitFor([&] { return writer.writtenRecords() == 1; });
    if (!explicit_request) {
        assert(completed_syncs.load() == 1);
    }
    waitFor([&] { return completed_syncs.load() == 2; });
    assert(writer.enabled());
    assert(!writer.stopRequested());
    assert(!writer.workerExited());
    assertRecords(config.directory, 1);
    assert(writer.stop(3s));
}

void testRequestDuringSync()
{
    auto config = configFor("request-during-sync");
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    std::atomic<int> completed_syncs{0};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::Sync && completed_syncs.load() == 1) {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return released; });
        }
        if (operation == Operation::SyncComplete) {
            completed_syncs.fetch_add(1);
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalTickEvent(1, 5.0);
    writer.requestFlush();
    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, 3s, [&] { return entered; }));
    }
    writer.journalTickEvent(2, 5.0);
    writer.requestFlush();
    {
        std::scoped_lock lock(mutex);
        released = true;
        cv.notify_all();
    }
    waitFor([&] { return completed_syncs.load() == 3; });
    assert(writer.enabled());
    assert(!writer.stopRequested());
    assertRecords(config.directory, 2);
    assert(writer.stop(3s));
}

void testFlushCoversBacklog()
{
    auto config = configFor("flush-backlog");
    config.flush_interval_ms = 60000;
    Gate gate;
    std::atomic<int> syncs{0};
    std::atomic<std::uint64_t> writes{0};
    std::atomic<std::uint64_t> synced_records{0};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::DrainComplete && syncs.load() == 1 && writes.load() == 0) {
            gate.enter();
        }
        if (operation == Operation::WriteComplete && syncs.load() != 0) {
            writes.fetch_add(1);
        }
        if (operation == Operation::SyncComplete) {
            synced_records.store(writes.load());
            syncs.fetch_add(1);
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    spark::RecoveryWriterQueueTestAccess::reserve(writer);
    assert(writer.start());
    gate.waitEntered();
    for (std::uint64_t tick = 1; tick <= 600; ++tick) {
        writer.journalTickEvent(tick, 5.0);
    }
    spark::RecoveryWriterQueueTestAccess::cancelReservation(writer);
    writer.requestFlush();
    gate.release();
    waitFor([&] { return synced_records.load() == 600; });
    assert(syncs.load() >= 4);
    assert(writer.enabled());
    assert(!writer.stopRequested());
    assertRecords(config.directory, 600);
    assert(writer.stop(3s));
    std::cout << "M1 pre-request 600-record backlog: PASS\n";
}

void testUnpublishedReservationDoesNotResync()
{
    auto config = configFor("flush-reservation");
    Gate write_gate;
    Gate drain_gate;
    std::atomic<int> syncs{0};
    std::atomic<int> empty_batches{0};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::Write && syncs.load() == 1) {
            write_gate.enter();
        }
        if (operation == Operation::SyncComplete) {
            syncs.fetch_add(1);
        }
        if (operation == Operation::DrainComplete && syncs.load() >= 2 && empty_batches.fetch_add(1) == 100) {
            drain_gate.enter();
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    spark::RecoveryWriterQueueTestAccess::reserve(writer);
    writer.journalTickEvent(1, 5.0);
    write_gate.waitEntered();
    writer.requestFlush();
    write_gate.release();
    drain_gate.waitEntered();
    assert(syncs.load() == 2);
    spark::RecoveryWriterQueueTestAccess::publishReservedTick(writer, 2);
    drain_gate.release();
    waitFor([&] { return syncs.load() == 3; });
    assert(writer.enabled());
    assertRecords(config.directory, 2);
    assert(writer.stop(3s));
    std::cout << "M1 unpublished reservation, 100 empty batches without sync churn: PASS\n";
}

void testRequestAtWaitBoundary()
{
    auto config = configFor("wait-boundary");
    config.flush_interval_ms = 60000;
    Gate wait_gate;
    Gate request_gate;
    std::mutex completion_mutex;
    std::condition_variable completion_cv;
    bool request_returned = false;
    std::atomic<int> syncs{0};
    std::atomic<bool> written{false};
    std::atomic<bool> armed{false};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::WriteComplete && syncs.load() == 1) {
            written.store(true);
        }
        if (operation == Operation::SyncComplete) {
            syncs.fetch_add(1);
        }
        if (operation == Operation::WaitBeforePark && written.load()) {
            wait_gate.enter();
        }
        if (operation == Operation::FlushRequestBeforeLock && armed.load()) {
            request_gate.enter();
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    spark::RecoveryWriterQueueTestAccess::reserve(writer);
    assert(writer.start());
    spark::RecoveryWriterQueueTestAccess::publishReservedTick(writer, 1);
    wait_gate.waitEntered();
    armed.store(true);
    std::thread requester([&] {
        writer.requestFlush();
        std::scoped_lock lock(completion_mutex);
        request_returned = true;
        completion_cv.notify_all();
    });
    request_gate.waitEntered();
    request_gate.release();
    {
        std::unique_lock lock(completion_mutex);
        assert(!completion_cv.wait_for(lock, 100ms, [&] { return request_returned; }));
    }
    wait_gate.release();
    {
        std::unique_lock lock(completion_mutex);
        assert(completion_cv.wait_for(lock, 3s, [&] { return request_returned; }));
    }
    requester.join();
    waitFor([&] { return syncs.load() == 2; });
    assert(writer.enabled());
    assert(!writer.stopRequested());
    assertRecords(config.directory, 1);
    assert(writer.stop(3s));
    std::cout << "M2 false-predicate/park boundary with concurrent request: PASS\n";
}

void testFailedSyncState(Operation failure)
{
    auto config = configFor("failed-sync-" + std::to_string(static_cast<int>(failure)));
    std::atomic<bool> armed{false};
    std::atomic<int> sync_attempts{0};
    config.io_hook = [&](Operation operation) {
        if (armed.load() && operation == Operation::Sync) {
            sync_attempts.fetch_add(1);
        }
        return !armed.load() || operation != failure;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    const auto previous_sync = spark::RecoveryWriterQueueTestAccess::lastSync(writer);
    armed.store(true);
    writer.journalTickEvent(1, 5.0);
    writer.requestFlush();
    waitFor([&] { return writer.workerExited(); });
    assert(writer.tryReap());
    assert(spark::RecoveryWriterQueueTestAccess::dirty(writer));
    assert(spark::RecoveryWriterQueueTestAccess::lastSync(writer) == previous_sync);
    assert(sync_attempts.load() == (failure == Operation::Flush ? 0 : 1));
}

void testQueueCannotStarveSync()
{
    auto config = configFor("queue-sync-deadline");
    config.sync_interval_ms = 40;
    std::atomic<int> completed_syncs{0};
    std::atomic<int> completed_closes{0};
    std::atomic<bool> first_data_write_gated{false};
    std::atomic<std::int64_t> first_data_write_entry_ns{0};
    Gate first_data_write_gate;
    Gate second_sync_complete_gate;
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::SyncComplete) {
            const int completed = completed_syncs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (completed == 2) {
                second_sync_complete_gate.enter();
            }
        }
        if (operation == Operation::CloseComplete) {
            completed_closes.fetch_add(1, std::memory_order_relaxed);
        }
        if (operation == Operation::Write && completed_syncs.load(std::memory_order_relaxed) == 1 &&
            !first_data_write_gated.exchange(true, std::memory_order_acq_rel)) {
            first_data_write_entry_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count(),
                                            std::memory_order_release);
            first_data_write_gate.enter();
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    assert(writer.enabled());
    writer.journalTickEvent(1, 5.0);
    first_data_write_gate.waitEntered();
    for (std::uint64_t tick = 2; tick <= 512; ++tick) {
        writer.journalTickEvent(tick, 5.0);
    }

    const auto entry_ns = first_data_write_entry_ns.load(std::memory_order_acquire);
    const auto first_write_entered = std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(entry_ns)));
    const auto first_write_release_deadline = first_write_entered + 40ms;
    std::this_thread::sleep_until(first_write_release_deadline);
    const auto first_write_gate_elapsed = std::chrono::steady_clock::now() - first_write_entered;
    const auto queued_before_first_write_release = spark::RecoveryWriterQueueTestAccess::queued(writer);
    const auto dropped_before_first_write_release = writer.droppedRecords();
    const auto written_before_first_write_release = writer.writtenRecords();
    const auto admission_before_first_write_release = writer.enabled();
    const auto stop_requested_before_first_write_release = writer.stopRequested();
    first_data_write_gate.release();

    second_sync_complete_gate.waitEntered();
    const auto syncs_at_nonempty_sync = completed_syncs.load(std::memory_order_relaxed);
    const auto written_at_nonempty_sync = writer.writtenRecords();
    const auto queued_at_nonempty_sync = spark::RecoveryWriterQueueTestAccess::queued(writer);
    const auto dropped_at_nonempty_sync = writer.droppedRecords();
    const auto enabled_at_nonempty_sync = writer.enabled();
    const auto stop_requested_at_nonempty_sync = writer.stopRequested();
    writer.requestStop();
    second_sync_complete_gate.release();

    const bool stopped = writer.stop(3s);
    const auto worker_exited = writer.workerExited();
    const auto final_sync_completions = completed_syncs.load(std::memory_order_relaxed);
    const auto final_close_completions = completed_closes.load(std::memory_order_relaxed);
    std::fprintf(
        stderr,
        "queue_sync_nonempty proof=deadline sync_interval_ms=40 first_write_hold_ms=%lld "
        "accepted=512 sync_completions=%d written=%llu queued=%llu dropped=%llu enabled=%d "
        "stop_requested_before=%d\n",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(first_write_gate_elapsed).count()),
        syncs_at_nonempty_sync, static_cast<unsigned long long>(written_at_nonempty_sync),
        static_cast<unsigned long long>(queued_at_nonempty_sync),
        static_cast<unsigned long long>(dropped_at_nonempty_sync), enabled_at_nonempty_sync ? 1 : 0,
        stop_requested_at_nonempty_sync ? 1 : 0);
    std::fprintf(stderr,
                 "queue_sync_final accepted=512 written=%llu queued=%llu dropped=%llu enabled=%d worker_exited=%d "
                 "stop3=%d final_sync_completions=%d final_close_completions=%d\n",
                 static_cast<unsigned long long>(writer.writtenRecords()),
                 static_cast<unsigned long long>(spark::RecoveryWriterQueueTestAccess::queued(writer)),
                 static_cast<unsigned long long>(writer.droppedRecords()), writer.enabled() ? 1 : 0,
                 worker_exited ? 1 : 0, stopped ? 1 : 0, final_sync_completions, final_close_completions);
    std::fflush(stderr);

    assert(first_write_gate_elapsed >= 40ms);
    assert(queued_before_first_write_release == 511);
    assert(dropped_before_first_write_release == 0);
    assert(written_before_first_write_release == 0);
    assert(admission_before_first_write_release);
    assert(!stop_requested_before_first_write_release);
    assert(syncs_at_nonempty_sync == 2);
    assert(written_at_nonempty_sync == 1);
    assert(queued_at_nonempty_sync == 511);
    assert(dropped_at_nonempty_sync == 0);
    assert(enabled_at_nonempty_sync);
    assert(!stop_requested_at_nonempty_sync);
    assert(stopped);
    assert(worker_exited);
    assert(writer.writtenRecords() == 512);
    assert(spark::RecoveryWriterQueueTestAccess::queued(writer) == 0);
    assert(writer.droppedRecords() == 0);
    assert(final_sync_completions == 3);
    assert(final_close_completions == 2);
    assertRecords(config.directory, 512);
}

void testRollingPublication()
{
    auto config = configFor("rolling-publication");
    config.max_segment_bytes = 256;
    config.max_total_bytes = 512;
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 100, "Server thread");
    for (std::uint64_t i = 1; i <= 200; ++i) {
        writer.journalTickEvent(i, 5.0);
    }
    assert(writer.stop(3s));
    assert(!std::filesystem::exists(config.directory / "segment-0.jnl"));
    const auto journal = spark::JournalReader::readSession(config.directory);
    assert(journal.valid);
    assert(!journal.fatal_error);
    assert(!journal.head_truncated);
    if (!journal.metadata_snapshot) {
        std::abort();
    }
    assert(journal.metadata_snapshot->valid);
    assert(journal.metadata_snapshot->modules.size() == 1);
    assert(journal.metadata_snapshot->threads.size() == 1);
    assert(journal.record_count > 0);
    assert(journal.record_count < 200);
}

void testRotationFault(Operation failure, int occurrence)
{
    auto config = configFor("rotation-" + std::to_string(static_cast<int>(failure)) + "-" + std::to_string(occurrence));
    config.max_segment_bytes = 1;
    config.max_total_bytes = 1;
    std::atomic<bool> armed{false};
    int seen = 0;
    int writes = 0;
    int partials = 0;
    int closes = 0;
    int publications = 0;
    config.io_hook = [&](Operation operation) {
        if (!armed.load()) {
            return true;
        }
        writes += operation == Operation::WriteComplete ? 1 : 0;
        partials += operation == Operation::PartialWriteComplete ? 1 : 0;
        closes += operation == Operation::Close ? 1 : 0;
        publications += operation == Operation::RenameComplete ? 1 : 0;
        return operation != failure || ++seen != occurrence;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    armed.store(true);
    writer.journalTickEvent(1, 5.0);
    assert(writer.stop(3s));
    assert(seen >= occurrence);
    assert(std::filesystem::exists(config.directory / "segment-0.jnl"));
    assert(!std::filesystem::exists(config.directory / "metadata.snapshot"));
    assert(!std::filesystem::exists(config.directory / "metadata.snapshot.tmp"));
    assert(!std::filesystem::exists(config.directory / "segment-1.jnl.tmp"));
    const auto journal = spark::JournalReader::readSession(config.directory);
    assert(journal.valid);
    assert(!journal.fatal_error);
    const bool active_write_failed =
        (failure == Operation::Write || failure == Operation::PartialWrite) && occurrence == 1;
    assert(std::cmp_equal(journal.record_count, active_write_failed ? 0 : 1));
    assert(partials == (failure == Operation::PartialWrite ? 1 : 0));
    if (failure == Operation::PartialWrite) {
        assert(writes == occurrence - 1);
        if (occurrence == 1) {
            assert(std::filesystem::file_size(config.directory / "segment-0.jnl") > spark::kFileHeaderSize);
            assert(journal.tail_truncated);
        }
    }
    if (occurrence == 3 || (failure == Operation::Rename && occurrence == 2)) {
        assert(closes == 4);
        assert(publications == 1);
    }
    else if (failure == Operation::Reopen) {
        assert(closes == 2);
        assert(publications == 1);
    }
    else if (failure == Operation::Rename || occurrence == 2) {
        assert(closes == 2);
        assert(publications == 0);
    }
    else {
        assert(closes == 1);
        assert(publications == 0);
    }
}

void testSnapshotDoesNotAcknowledgeActiveData()
{
    auto config = configFor("snapshot-state");
    std::atomic<bool> fail_sync{false};
    config.io_hook = [&](Operation operation) {
        return !fail_sync.load() || operation != Operation::Sync;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    fail_sync.store(true);
    writer.journalTickEvent(1, 5.0);
    writer.requestFlush();
    waitFor([&] { return writer.workerExited(); });
    assert(writer.tryReap());
    const auto previous_sync = spark::RecoveryWriterQueueTestAccess::lastSync(writer);
    fail_sync.store(false);
    assert(spark::RecoveryWriterQueueTestAccess::snapshot(writer));
    assert(spark::RecoveryWriterQueueTestAccess::dirty(writer));
    assert(spark::RecoveryWriterQueueTestAccess::lastSync(writer) == previous_sync);
}

std::string readBytes(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void testPriorSnapshotSurvives(Operation failure)
{
    auto config = configFor("prior-snapshot-" + std::to_string(static_cast<int>(failure)));
    config.max_segment_bytes = 92;
    config.max_total_bytes = 150;
    Gate first_snapshot;
    std::atomic<bool> armed{false};
    int renames = 0;
    int failures = 0;
    int partials = 0;
    config.io_hook = [&](Operation operation) {
        if (!armed.load()) {
            if (operation == Operation::RenameComplete && ++renames == 4) {
                first_snapshot.enter();
            }
            return true;
        }
        if (operation == Operation::PartialWriteComplete) {
            ++partials;
        }
        const bool is_snapshot = std::filesystem::exists(config.directory / "metadata.snapshot.tmp");
        if (is_snapshot && operation == failure) {
            ++failures;
            return false;
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    for (std::uint64_t tick = 1; tick <= 4; ++tick) {
        writer.journalTickEvent(tick, 5.0);
    }
    writer.requestFlush();
    first_snapshot.waitEntered();
    const auto snapshot = readBytes(config.directory / "metadata.snapshot");
    const auto retained = readBytes(config.directory / "segment-1.jnl");
    assert(!snapshot.empty());
    armed.store(true);
    writer.journalTickEvent(5, 5.0);
    writer.journalTickEvent(6, 5.0);
    first_snapshot.release();
    assert(writer.stop(3s));
    assert(failures == 1);
    assert(partials == (failure == Operation::PartialWrite ? 1 : 0));
    assert(readBytes(config.directory / "metadata.snapshot") == snapshot);
    assert(readBytes(config.directory / "segment-1.jnl") == retained);
    assert(std::filesystem::exists(config.directory / "segment-2.jnl"));
    assert(std::filesystem::exists(config.directory / "segment-3.jnl"));
    assert(!std::filesystem::exists(config.directory / "metadata.snapshot.tmp"));
    const auto journal = spark::JournalReader::readSession(config.directory);
    assert(journal.valid && !journal.fatal_error && !journal.head_truncated);
    assert(journal.record_count == 4);
}

void testPersistentSnapshotFailureStopsGrowth(Operation failure)
{
    auto config = configFor("persistent-snapshot-" + std::to_string(static_cast<int>(failure)));
    const auto tick_size =
        spark::serializeRecord(spark::RecordType::TickEvent, 0, spark::buildTickEventPayload(1, 5.0)).size();
    config.max_segment_bytes = spark::kFileHeaderSize + tick_size;
    config.max_total_bytes = spark::kFileHeaderSize;

    std::atomic<int> snapshot_attempts{0};
    std::atomic<int> injected_failures{0};
    std::atomic<std::uintmax_t> segment_bytes_at_threshold{0};
    std::atomic<bool> all_records_drained{false};
    spark::RecoveryWriter *writer_ptr = nullptr;
    config.io_hook = [&](Operation operation) {
        const auto tmp = config.directory / "metadata.snapshot.tmp";
        std::error_code ec;
        const bool snapshot_tmp_exists = std::filesystem::exists(tmp, ec) && !ec;
        const bool is_snapshot_operation = operation == Operation::SnapshotOpen || snapshot_tmp_exists;
        if (operation == Operation::SnapshotOpen) {
            snapshot_attempts.fetch_add(1, std::memory_order_relaxed);
        }
        if (is_snapshot_operation && operation == failure) {
            const int failed = injected_failures.fetch_add(1, std::memory_order_relaxed) + 1;
            if (failed == 4) {
                segment_bytes_at_threshold.store(segmentFileBytes(config.directory), std::memory_order_relaxed);
            }
            return false;
        }
        if (operation == Operation::DrainComplete && writer_ptr != nullptr && writer_ptr->writtenRecords() == 64) {
            all_records_drained.store(true, std::memory_order_release);
        }
        return true;
    };

    spark::RecoveryWriter writer(config);
    writer_ptr = &writer;
    assert(writer.start());
    for (std::uint64_t tick = 1; tick <= 64; ++tick) {
        writer.journalTickEvent(tick, 5.0);
    }
    writer.requestFlush();
    if (failure == Operation::Close) {
        for (int expected_failures = 1; expected_failures <= 4; ++expected_failures) {
            const auto stage_started = std::chrono::steady_clock::now();
            const auto deadline = stage_started + 3s;
            while (injected_failures.load(std::memory_order_relaxed) < expected_failures &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stage_started);
            const bool stage_ready = injected_failures.load(std::memory_order_relaxed) >= expected_failures;
            std::fprintf(stderr,
                         "snapshot_progress stage=%d attempts=%d failures=%d written=%llu enabled=%d "
                         "worker_exited=%d elapsed_ms=%lld ready=%d\n",
                         expected_failures, snapshot_attempts.load(std::memory_order_relaxed),
                         injected_failures.load(std::memory_order_relaxed),
                         static_cast<unsigned long long>(writer.writtenRecords()), writer.enabled() ? 1 : 0,
                         writer.workerExited() ? 1 : 0, static_cast<long long>(elapsed.count()), stage_ready ? 1 : 0);
            if (!stage_ready) {
                std::fprintf(stderr,
                             "snapshot_progress_timeout stage=failure-%d attempts=%d failures=%d written=%llu "
                             "enabled=%d worker_exited=%d\n",
                             expected_failures, snapshot_attempts.load(std::memory_order_relaxed),
                             injected_failures.load(std::memory_order_relaxed),
                             static_cast<unsigned long long>(writer.writtenRecords()), writer.enabled() ? 1 : 0,
                             writer.workerExited() ? 1 : 0);
                std::fflush(stderr);
            }
            assert(stage_ready);
        }

        const auto completion_started = std::chrono::steady_clock::now();
        const auto completion_deadline = completion_started + 3s;
        const auto completion_ready = [&] {
            return (!writer.enabled() || all_records_drained.load(std::memory_order_acquire)) && writer.workerExited();
        };
        while (!completion_ready() && std::chrono::steady_clock::now() < completion_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        const auto completion_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - completion_started);
        const bool completed = completion_ready();
        std::fprintf(stderr,
                     "snapshot_progress stage=completion attempts=%d failures=%d written=%llu enabled=%d "
                     "worker_exited=%d all_records_drained=%d elapsed_ms=%lld ready=%d\n",
                     snapshot_attempts.load(std::memory_order_relaxed),
                     injected_failures.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(writer.writtenRecords()), writer.enabled() ? 1 : 0,
                     writer.workerExited() ? 1 : 0, all_records_drained.load(std::memory_order_acquire) ? 1 : 0,
                     static_cast<long long>(completion_elapsed.count()), completed ? 1 : 0);
        if (!completed) {
            std::fprintf(stderr,
                         "snapshot_completion_timeout attempts=%d failures=%d written=%llu enabled=%d "
                         "worker_exited=%d all_records_drained=%d\n",
                         snapshot_attempts.load(std::memory_order_relaxed),
                         injected_failures.load(std::memory_order_relaxed),
                         static_cast<unsigned long long>(writer.writtenRecords()), writer.enabled() ? 1 : 0,
                         writer.workerExited() ? 1 : 0, all_records_drained.load(std::memory_order_acquire) ? 1 : 0);
            std::fflush(stderr);
        }
        assert(completed);
    }
    else {
        waitFor([&] {
            return injected_failures.load(std::memory_order_relaxed) >= 4 &&
                   (!writer.enabled() || all_records_drained.load(std::memory_order_acquire));
        });
    }
    const auto actual_segment_bytes = segmentFileBytes(config.directory);
    const auto regular_bytes = regularFileBytes(config.directory);
    std::fprintf(stderr,
                 "snapshot_growth_red operation=%d snapshot_attempts=%d failures=%d written=%llu max_total=%llu "
                 "writer_total_bytes=%zu totalactualsegmentbytes=%llu regularbytes=%llu\n",
                 static_cast<int>(failure), snapshot_attempts.load(std::memory_order_relaxed),
                 injected_failures.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(writer.writtenRecords()),
                 static_cast<unsigned long long>(config.max_total_bytes),
                 spark::RecoveryWriterQueueTestAccess::totalBytes(writer),
                 static_cast<unsigned long long>(actual_segment_bytes), static_cast<unsigned long long>(regular_bytes));
    std::fflush(stderr);
    assert(!writer.enabled());
    assert(writer.journalDegraded());
    assert(writer.journalDegradationReason() == "snapshot_failed");
    assert(injected_failures.load(std::memory_order_relaxed) == 4);
    assert(snapshot_attempts.load(std::memory_order_relaxed) == 4);
    assert(writer.writtenRecords() == 4);
    assert(writer.stop(3s));

    const auto bytes_at_threshold = segment_bytes_at_threshold.load(std::memory_order_relaxed);
    const auto bytes_after_shutdown = segmentFileBytes(config.directory);
    const auto bounded_failure_bytes = 5 * spark::kFileHeaderSize + 4 * tick_size;
    std::fprintf(stderr,
                 "snapshot_growth_poststop operation=%d bytes_at_threshold=%llu bytes_after_shutdown=%llu "
                 "maxexpected=%llu\n",
                 static_cast<int>(failure), static_cast<unsigned long long>(bytes_at_threshold),
                 static_cast<unsigned long long>(bytes_after_shutdown),
                 static_cast<unsigned long long>(bounded_failure_bytes));
    std::fflush(stderr);
    assert(bytes_at_threshold != 0);
    assert(bytes_after_shutdown == bytes_at_threshold);
    writer.journalTickEvent(65, 5.0);
    assert(writer.stop(3s));
    assert(segmentFileBytes(config.directory) == bytes_after_shutdown);

    assert(bytes_after_shutdown == bounded_failure_bytes);
}

void testSnapshotCloseObservationProof()
{
    using Clock = std::chrono::steady_clock;

    auto config = configFor("snapshot-close-observation-proof");
    const auto tick_size =
        spark::serializeRecord(spark::RecordType::TickEvent, 0, spark::buildTickEventPayload(1, 5.0)).size();
    config.max_segment_bytes = spark::kFileHeaderSize + tick_size;
    config.max_total_bytes = spark::kFileHeaderSize;

    SnapshotAttemptGate close_gate;
    std::atomic<int> snapshot_attempts{0};
    std::atomic<int> injected_failures{0};
    std::atomic<std::uintmax_t> segment_bytes_at_threshold{0};
    std::atomic<bool> all_records_drained{false};
    spark::RecoveryWriter *writer_ptr = nullptr;
    config.io_hook = [&](Operation operation) {
        const auto tmp = config.directory / "metadata.snapshot.tmp";
        std::error_code ec;
        const bool is_snapshot = operation == Operation::SnapshotOpen || (std::filesystem::exists(tmp, ec) && !ec);
        if (operation == Operation::SnapshotOpen) {
            snapshot_attempts.fetch_add(1, std::memory_order_relaxed);
        }
        if (is_snapshot && operation == Operation::Close) {
            const int failure = injected_failures.fetch_add(1, std::memory_order_relaxed) + 1;
            if (failure == 4) {
                segment_bytes_at_threshold.store(segmentFileBytes(config.directory), std::memory_order_relaxed);
            }
            close_gate.enter(failure, snapshot_attempts.load(std::memory_order_relaxed), writer_ptr->writtenRecords());
            return false;
        }
        if (operation == Operation::DrainComplete && writer_ptr != nullptr && writer_ptr->writtenRecords() == 64) {
            all_records_drained.store(true, std::memory_order_release);
        }
        return true;
    };

    spark::RecoveryWriter writer(config);
    writer_ptr = &writer;
    assert(writer.start());
    for (std::uint64_t tick = 1; tick <= 64; ++tick) {
        writer.journalTickEvent(tick, 5.0);
    }
    writer.requestFlush();

    const bool first_attempt_entered = close_gate.waitEntered(1);
    if (!first_attempt_entered) {
        close_gate.releaseAll();
        const bool stopped = writer.stop(3s);
        std::fprintf(stderr, "snapshot_close_proof_setup_failed entered=%zu failures=%d written=%llu stopped=%d\n",
                     close_gate.enteredCount(), injected_failures.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(writer.writtenRecords()), stopped ? 1 : 0);
        std::fflush(stderr);
        assert(first_attempt_entered);
        return;
    }

    struct OldObserverResult {
        Clock::time_point started;
        Clock::time_point completed;
        bool predicate_satisfied = false;
        int attempts = 0;
        int failures = 0;
        std::uint64_t written = 0;
        bool enabled = false;
        bool worker_exited = false;
        bool records_drained = false;
    } observer_result;
    std::mutex observer_mutex;
    std::condition_variable observer_cv;
    bool observer_started = false;
    std::thread observer([&] {
        observer_result.started = Clock::now();
        {
            std::scoped_lock lock(observer_mutex);
            observer_started = true;
        }
        observer_cv.notify_all();

        const auto deadline = observer_result.started + 3s;
        const auto old_predicate = [&] {
            return injected_failures.load(std::memory_order_relaxed) >= 4 &&
                   (!writer.enabled() || all_records_drained.load(std::memory_order_acquire));
        };
        while (Clock::now() < deadline && !old_predicate()) {
            std::this_thread::sleep_for(1ms);
        }
        observer_result.predicate_satisfied = old_predicate();
        observer_result.completed = Clock::now();
        observer_result.attempts = snapshot_attempts.load(std::memory_order_relaxed);
        observer_result.failures = injected_failures.load(std::memory_order_relaxed);
        observer_result.written = writer.writtenRecords();
        observer_result.enabled = writer.enabled();
        observer_result.worker_exited = writer.workerExited();
        observer_result.records_drained = all_records_drained.load(std::memory_order_acquire);
    });

    bool observer_started_ok = false;
    {
        std::unique_lock lock(observer_mutex);
        observer_started_ok = observer_cv.wait_for(lock, 3s, [&] { return observer_started; });
    }
    if (!observer_started_ok) {
        close_gate.releaseAll();
    }

    bool controlled_progress = observer_started_ok;
    for (std::size_t attempt = 1; controlled_progress && attempt <= 4; ++attempt) {
        if (attempt > 1 && !close_gate.waitEntered(attempt)) {
            controlled_progress = false;
            break;
        }
        const auto stage = close_gate.attempt(attempt - 1);
        auto release_at = stage.entered + 1s;
        if (attempt == 1) {
            release_at = std::max(release_at, observer_result.started + 1s);
        }
        std::this_thread::sleep_until(release_at);
        close_gate.releaseThrough(attempt);
        if (!close_gate.waitCompleted(attempt)) {
            controlled_progress = false;
        }
    }
    close_gate.releaseAll();
    observer.join();

    const auto worker_deadline = Clock::now() + 3s;
    while (!writer.workerExited() && Clock::now() < worker_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool stop_succeeded = writer.stop(3s);
    const auto final_segment_bytes = segmentFileBytes(config.directory);
    const auto actual_regular_bytes = regularFileBytes(config.directory);
    const auto expected_bytes = 5 * spark::kFileHeaderSize + 4 * tick_size;
    const auto gate_entry_count = close_gate.enteredCount();
    const auto gate_completed_count = close_gate.completedCount();

    const auto to_us = [](Clock::time_point time) {
        return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
    };
    for (std::size_t i = 0; i < gate_entry_count && i < 4; ++i) {
        const auto attempt = close_gate.attempt(i);
        const auto hold_ms = std::chrono::duration_cast<std::chrono::milliseconds>(attempt.released - attempt.entered);
        std::fprintf(stderr,
                     "snapshot_close_proof_attempt index=%zu entered_us=%lld released_us=%lld hold_ms=%lld "
                     "failures=%d snapshot_attempts=%d written=%llu\n",
                     i + 1, static_cast<long long>(to_us(attempt.entered)),
                     static_cast<long long>(to_us(attempt.released)), static_cast<long long>(hold_ms.count()),
                     attempt.failures, attempt.snapshot_attempts, static_cast<unsigned long long>(attempt.written));
    }
    const auto observer_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(observer_result.completed - observer_result.started);
    std::fprintf(stderr,
                 "snapshot_close_proof_deadline deadline_ms=3000 elapsed_ms=%lld predicate_satisfied=%d "
                 "attempts=%d failures=%d written=%llu enabled=%d worker_exited=%d records_drained=%d\n",
                 static_cast<long long>(observer_elapsed.count()), observer_result.predicate_satisfied ? 1 : 0,
                 observer_result.attempts, observer_result.failures,
                 static_cast<unsigned long long>(observer_result.written), observer_result.enabled ? 1 : 0,
                 observer_result.worker_exited ? 1 : 0, observer_result.records_drained ? 1 : 0);
    std::fprintf(stderr,
                 "snapshot_close_proof_final controlled_progress=%d gates=%llu completed_gates=%llu gate_timeout=%d "
                 "unexpected_gate=%d attempts=%d failures=%d written=%llu enabled=%d degradation=%s "
                 "worker_exited=%d bytes_at_threshold=%llu final_segment_bytes=%llu regular_bytes=%llu "
                 "expected_bytes=%llu dropped=%llu stop3=%d\n",
                 controlled_progress ? 1 : 0, static_cast<unsigned long long>(gate_entry_count),
                 static_cast<unsigned long long>(gate_completed_count), close_gate.timedOut() ? 1 : 0,
                 close_gate.unexpectedEntry() ? 1 : 0, snapshot_attempts.load(std::memory_order_relaxed),
                 injected_failures.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(writer.writtenRecords()), writer.enabled() ? 1 : 0,
                 writer.journalDegradationReason().c_str(), writer.workerExited() ? 1 : 0,
                 static_cast<unsigned long long>(segment_bytes_at_threshold.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(final_segment_bytes),
                 static_cast<unsigned long long>(actual_regular_bytes), static_cast<unsigned long long>(expected_bytes),
                 static_cast<unsigned long long>(writer.droppedRecords()), stop_succeeded ? 1 : 0);
    std::fflush(stderr);

    assert(observer_started_ok);
    assert(controlled_progress);
    assert(!observer_result.predicate_satisfied);
    assert(observer_elapsed >= 3s);
    assert(gate_entry_count == 4);
    assert(gate_completed_count == 4);
    assert(!close_gate.timedOut());
    assert(!close_gate.unexpectedEntry());
    assert(std::chrono::duration_cast<std::chrono::milliseconds>(close_gate.attempt(3).released -
                                                                 observer_result.started) > 3s);
    for (std::size_t i = 0; i < 4; ++i) {
        const auto attempt = close_gate.attempt(i);
        const auto hold = attempt.released - attempt.entered;
        assert(hold >= 1s);
        assert(hold < 3s);
        assert(std::cmp_equal(attempt.failures, i + 1));
        assert(std::cmp_equal(attempt.snapshot_attempts, i + 1));
        assert(attempt.written == i + 1);
    }
    assert(stop_succeeded);
    assert(writer.workerExited());
    assert(!writer.enabled());
    assert(writer.journalDegraded());
    assert(writer.journalDegradationReason() == "snapshot_failed");
    assert(snapshot_attempts.load(std::memory_order_relaxed) == 4);
    assert(injected_failures.load(std::memory_order_relaxed) == 4);
    assert(writer.writtenRecords() == 4);
    assert(writer.droppedRecords() == 0);
    assert(segment_bytes_at_threshold.load(std::memory_order_relaxed) == expected_bytes);
    assert(final_segment_bytes == expected_bytes);
    assert(actual_regular_bytes == expected_bytes);
    writer.journalTickEvent(65, 5.0);
    assert(writer.stop(3s));
    assert(segmentFileBytes(config.directory) == expected_bytes);
    std::cout << "P1 controlled Close snapshot failures outlive the original observer deadline: PASS\n";
}

void testBlockedStopOwnershipProof()
{
    auto config = configFor("blocked-stop-ownership-proof");
    config.queue_capacity = 16;
    Gate write_gate;
    std::atomic<bool> arm_write_gate{false};
    std::atomic<int> completed_syncs{0};
    std::atomic<int> completed_closes{0};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::SyncComplete) {
            completed_syncs.fetch_add(1, std::memory_order_relaxed);
        }
        if (operation == Operation::CloseComplete) {
            completed_closes.fetch_add(1, std::memory_order_relaxed);
        }
        if (operation == Operation::Write && arm_write_gate.exchange(false, std::memory_order_acq_rel)) {
            write_gate.enter();
        }
        return true;
    };

    spark::RecoveryWriter writer(config);
    assert(writer.start());
    arm_write_gate.store(true, std::memory_order_release);
    for (std::uint64_t tick = 1; tick <= 8; ++tick) {
        writer.journalTickEvent(tick, 5.0);
    }
    write_gate.waitEntered();

    const auto stop_started = std::chrono::steady_clock::now();
    const bool stop_before_release = writer.stop(20ms);
    const auto stop_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stop_started);
    const auto queued_before_release = spark::RecoveryWriterQueueTestAccess::queued(writer);
    const auto written_before_release = writer.writtenRecords();
    const auto dropped_before_release = writer.droppedRecords();
    const bool worker_exited_before_release = writer.workerExited();
    const bool reaped_before_release = writer.tryReap();
    writer.journalTickEvent(9, 5.0);
    const auto queued_after_rejected_record = spark::RecoveryWriterQueueTestAccess::queued(writer);
    std::fprintf(stderr,
                 "blocked_stop_proof operation=Write accepted=8 queued=%llu written=%llu dropped=%llu "
                 "stop_budget_ms=20 stop_result=%d elapsed_ms=%lld worker_exited=%d reaped=%d "
                 "queued_after_rejected=%llu\n",
                 static_cast<unsigned long long>(queued_before_release),
                 static_cast<unsigned long long>(written_before_release),
                 static_cast<unsigned long long>(dropped_before_release), stop_before_release ? 1 : 0,
                 static_cast<long long>(stop_elapsed.count()), worker_exited_before_release ? 1 : 0,
                 reaped_before_release ? 1 : 0, static_cast<unsigned long long>(queued_after_rejected_record));
    std::fflush(stderr);

    write_gate.release();
    const bool stop_after_release = writer.stop(3s);
    const bool worker_exited_after_release = writer.workerExited();
    const bool reaped_after_release = writer.tryReap();
    std::fprintf(stderr,
                 "blocked_stop_proof_released stop3=%d worker_exited=%d reaped=%d written=%llu queued=%llu "
                 "dropped=%llu sync_completions=%d close_completions=%d\n",
                 stop_after_release ? 1 : 0, worker_exited_after_release ? 1 : 0, reaped_after_release ? 1 : 0,
                 static_cast<unsigned long long>(writer.writtenRecords()),
                 static_cast<unsigned long long>(spark::RecoveryWriterQueueTestAccess::queued(writer)),
                 static_cast<unsigned long long>(writer.droppedRecords()),
                 completed_syncs.load(std::memory_order_relaxed), completed_closes.load(std::memory_order_relaxed));
    std::fflush(stderr);

    assert(!stop_before_release);
    assert(stop_elapsed >= 10ms);
    assert(stop_elapsed < 500ms);
    assert(!worker_exited_before_release);
    assert(!reaped_before_release);
    assert(queued_before_release == 7);
    assert(written_before_release == 0);
    assert(dropped_before_release == 0);
    assert(queued_after_rejected_record == queued_before_release);
    assert(stop_after_release);
    assert(worker_exited_after_release);
    assert(reaped_after_release);
    assert(writer.writtenRecords() == 8);
    assert(spark::RecoveryWriterQueueTestAccess::queued(writer) == 0);
    assert(writer.droppedRecords() == 0);
    assert(completed_syncs.load(std::memory_order_relaxed) == 2);
    assert(completed_closes.load(std::memory_order_relaxed) == 2);
    assertRecords(config.directory, 8);
    std::cout << "P1 bounded stop retains worker ownership until gated I/O is released: PASS\n";
}

void testPersistentPruneFailureStopsGrowth()
{
    auto config = configFor("persistent-prune");
    const auto tick_size =
        spark::serializeRecord(spark::RecordType::TickEvent, 0, spark::buildTickEventPayload(1, 5.0)).size();
    config.max_segment_bytes = spark::kFileHeaderSize + tick_size;
    config.max_total_bytes = spark::kFileHeaderSize;
    std::atomic<int> completed_renames{0};
    std::atomic<bool> obstructed{false};
    config.io_hook = [&](Operation operation) {
        if (operation != Operation::RenameComplete || completed_renames.fetch_add(1) + 1 != 3) {
            return true;
        }

        const auto segment = config.directory / "segment-0.jnl";
        const auto held = config.directory / "segment-0-held.jnl";
        std::error_code ec;
        std::filesystem::rename(segment, held, ec);
        assert(!ec);
        std::filesystem::create_directory(segment, ec);
        assert(!ec);
        std::ofstream blocker(segment / "blocker", std::ios::binary);
        assert(blocker);
        blocker.put('x');
        assert(blocker);
        obstructed.store(true, std::memory_order_release);
        return true;
    };

    spark::RecoveryWriter writer(config);
    assert(writer.start());
    for (std::uint64_t tick = 1; tick <= 64; ++tick) {
        writer.journalTickEvent(tick, 5.0);
    }
    writer.requestFlush();
    waitFor([&] { return obstructed.load(std::memory_order_acquire); });
    waitFor([&] { return !writer.journalDegradationReason().empty(); });
    waitFor([&] { return !writer.enabled() || writer.writtenRecords() == 64; });
    assert(!writer.enabled());
    assert(writer.journalDegradationReason() == "prune_stalled");
    const auto written_at_disable = writer.writtenRecords();
    const auto bytes_at_disable = regularFileBytes(config.directory);
    assert(written_at_disable < 64);
    assert(bytes_at_disable != std::numeric_limits<std::uintmax_t>::max());

    for (std::uint64_t tick = 65; tick <= 128; ++tick) {
        writer.journalTickEvent(tick, 5.0);
    }
    assert(writer.stop(3s));
    assert(writer.writtenRecords() == written_at_disable);
    assert(regularFileBytes(config.directory) == bytes_at_disable);
    std::cout << "P1 persistent prune failure stops journal growth: PASS\n";
}

void testMissingRetainedSegmentDoesNotOverPrune()
{
    auto config = configFor("missing-retained-segment");
    const auto tick_size =
        spark::serializeRecord(spark::RecordType::TickEvent, 0, spark::buildTickEventPayload(1, 5.0)).size();
    config.max_segment_bytes = spark::kFileHeaderSize + tick_size;
    config.max_total_bytes = 3 * config.max_segment_bytes + spark::kFileHeaderSize;

    Gate rotation_complete;
    std::atomic<bool> paused{false};
    spark::RecoveryWriter *writer_ptr = nullptr;
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::DrainComplete && writer_ptr != nullptr && writer_ptr->writtenRecords() == 4 &&
            !paused.exchange(true, std::memory_order_acq_rel)) {
            rotation_complete.enter();
        }
        return true;
    };

    spark::RecoveryWriter writer(config);
    writer_ptr = &writer;
    assert(writer.start());
    for (std::uint64_t tick = 1; tick <= 4; ++tick) {
        writer.journalTickEvent(tick, 5.0);
    }
    rotation_complete.waitEntered();
    const auto missing_segment = config.directory / "segment-1.jnl";
    assert(std::filesystem::exists(missing_segment));
    std::error_code ec;
    assert(std::filesystem::remove(missing_segment, ec));
    assert(!ec);
    rotation_complete.release();

    writer.journalTickEvent(5, 5.0);
    writer.requestFlush();
    waitFor([&] { return writer.writtenRecords() == 5; });
    assert(writer.stop(3s));
    assert(std::filesystem::exists(config.directory / "segment-2.jnl"));
    std::cout << "P2 missing segment retires accounted bytes without over-pruning: PASS\n";
}

void testConcurrentDegradationReportKeepsFirstCause()
{
    auto config = configFor("sticky-degradation-cause");
    std::mutex mutex;
    std::condition_variable cv;
    int entered = 0;
    bool release_first = false;
    bool release_second = false;
    config.io_hook = [&](Operation operation) {
        if (operation != Operation::DegradationBeforeLock) {
            return true;
        }
        std::unique_lock lock(mutex);
        ++entered;
        cv.notify_all();
        cv.wait(lock, [&] { return DegradationReporter == 1 ? release_first : release_second; });
        return true;
    };

    spark::RecoveryWriter writer(config);
    std::thread first([&] {
        DegradationReporter = 1;
        spark::RecoveryWriterQueueTestAccess::reportDegradation(writer, "write_failed");
    });
    std::thread second([&] {
        DegradationReporter = 2;
        spark::RecoveryWriterQueueTestAccess::reportDegradation(writer, "sync_failed");
    });
    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, 3s, [&] { return entered == 2; }));
        release_first = true;
        cv.notify_all();
    }
    waitFor([&] { return writer.journalDegraded(); });
    {
        std::scoped_lock lock(mutex);
        release_second = true;
        cv.notify_all();
    }
    first.join();
    second.join();
    assert(writer.journalDegradationReason() == "write_failed");
}

void testFinalCloseFailureIsReported()
{
    auto config = configFor("final-close-failure");
    std::atomic<int> closes{0};
    config.io_hook = [&](Operation operation) {
        return operation != Operation::Close || closes.fetch_add(1, std::memory_order_relaxed) + 1 != 2;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    assert(writer.stop(3s));
    assert(closes.load(std::memory_order_relaxed) == 2);
    assert(writer.journalDegraded());
    assert(writer.journalDegradationReason() == "close_failed");
}

void testStartupClearsReplayGuardAfterArtifactPurge()
{
    auto config = configFor("startup-replay-guard");
    std::error_code ec;
    const auto marker = spark::recoveryReplayGuardPath(config.directory);
    std::filesystem::remove(marker, ec);
    assert(!ec);
    assert(spark::blockRecoveryReplay(config.directory, ec));
    assert(!ec);

    const std::vector<std::string> artifacts{"segment-nonnumeric.jnl", "segment-.jnl.tmp", "metadata.snapshot",
                                             "metadata.snapshot.tmp"};
    for (const auto &name : artifacts) {
        std::ofstream output(config.directory / name, std::ios::binary);
        assert(output);
        output.put('x');
        assert(output);
    }

    bool checked_before_segment_publish = false;
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::Rename) {
            std::error_code guard_ec;
            assert(!spark::recoveryReplayBlocked(config.directory, guard_ec));
            assert(!guard_ec);
            for (const auto &name : artifacts) {
                assert(!std::filesystem::exists(config.directory / name));
            }
            checked_before_segment_publish = true;
        }
        return true;
    };

    spark::RecoveryWriter writer(config);
    assert(writer.start());
    assert(checked_before_segment_publish);
    assert(std::filesystem::exists(config.directory / "segment-0.jnl"));
    assert(writer.stop(3s));
}

void testStartupCleanupFailureKeepsReplayGuard()
{
    auto config = configFor("startup-cleanup-failure");
    std::error_code ec;
    const auto marker = spark::recoveryReplayGuardPath(config.directory);
    std::filesystem::remove(marker, ec);
    assert(!ec);
    assert(spark::blockRecoveryReplay(config.directory, ec));
    assert(!ec);

    const auto blocked_artifact = config.directory / "segment-invalid.jnl";
    std::filesystem::create_directory(blocked_artifact, ec);
    assert(!ec);
    std::ofstream blocker(blocked_artifact / "blocker", std::ios::binary);
    assert(blocker);
    blocker.put('x');
    assert(blocker);
    blocker.close();
    assert(blocker);

    spark::RecoveryWriter writer(config);
    assert(!writer.start());
    assert(!writer.enabled());
    assert(spark::recoveryReplayBlocked(config.directory, ec));
    assert(!ec);
    assert(std::filesystem::exists(blocked_artifact / "blocker"));

    std::filesystem::remove_all(blocked_artifact, ec);
    assert(!ec);
    assert(spark::clearRecoveryReplayBlock(config.directory, ec));
    assert(!ec);
}

void testStartupGuardClearFailureDoesNotPublishSegment()
{
    auto config = configFor("startup-guard-clear-failure");
    std::error_code ec;
    const auto marker = spark::recoveryReplayGuardPath(config.directory);
    std::filesystem::remove_all(marker, ec);
    assert(!ec);
    assert(std::filesystem::create_directory(marker, ec));
    assert(!ec);
    std::ofstream blocker(marker / "blocker", std::ios::binary);
    assert(blocker);
    blocker.put('x');
    assert(blocker);
    blocker.close();
    assert(blocker);

    spark::RecoveryWriter writer(config);
    assert(!writer.start());
    assert(!writer.enabled());
    assert(spark::recoveryReplayBlocked(config.directory, ec));
    assert(!ec);
    assert(std::filesystem::exists(marker / "blocker"));
    assert(!std::filesystem::exists(config.directory / "segment-0.jnl"));
    assert(!std::filesystem::exists(config.directory / "segment-0.jnl.tmp"));

    std::filesystem::remove_all(marker, ec);
    assert(!ec);
}

[[noreturn]] void crashChild(const std::filesystem::path &directory, const std::string &point)
{
    spark::RecoveryWriter::Config config;
    config.directory = directory;
    config.session_id = 1000;
    config.flush_interval_ms = 5;
    config.sync_interval_ms = 60000;
    config.max_segment_bytes =
        spark::kFileHeaderSize +
        2 * spark::serializeRecord(spark::RecordType::TickEvent, 0, spark::buildTickEventPayload(1, 5.0)).size();
    std::atomic<int> syncs{0};
    std::atomic<bool> rotating{false};
    int writes = 0;
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::SyncComplete) {
            syncs.fetch_add(1);
        }
        if (!rotating.load()) {
            return true;
        }
        if (operation == Operation::Write) {
            ++writes;
        }
        if ((point == "before-header" && operation == Operation::Write && writes == 2) ||
            (point == "partial-header" && writes == 2 && operation == Operation::CloseComplete) ||
            (point == "before-publish" && operation == Operation::Rename) ||
            (point == "after-publish" && operation == Operation::RenameComplete)) {
            std::_Exit(73);
        }
        return !(point == "partial-header" && writes == 2 && operation == Operation::PartialWrite);
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalTickEvent(1, 5.0);
    writer.requestFlush();
    waitFor([&] { return syncs.load() == 2; });
    if (point == "durable-record") {
        std::_Exit(73);
    }
    rotating.store(true);
    writer.journalTickEvent(2, 5.0);
    writer.requestFlush();
    std::this_thread::sleep_for(3s);
    std::_Exit(74);
}

void testCrash(const char *executable, const std::string &point)
{
    const auto directory = testDirectory("crash-" + point);
    const auto path = directory.string();
#ifdef _WIN32
    const char *arguments[]{executable, "--child", path.c_str(), point.c_str(), nullptr};
    assert(_spawnv(_P_WAIT, executable, arguments) == 73);
#else
    const auto child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(executable, executable, "--child", path.c_str(), point.c_str(), nullptr);
        std::_Exit(75);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 73);
#endif
    assertRecords(directory, point == "durable-record" ? 1 : 2);
    assert(std::filesystem::exists(directory / "segment-1.jnl") == (point == "after-publish"));
    if (point == "partial-header") {
        assert(std::filesystem::file_size(directory / "segment-1.jnl.tmp") == spark::kFileHeaderSize / 2);
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 4 && std::string(argv[1]) == "--child") {
        crashChild(argv[2], argv[3]);
    }
    const std::string group = argc == 2 ? argv[1] : "all";
    if (group == "--snapshot-growth") {
        for (const auto operation : {Operation::SnapshotOpen, Operation::Write, Operation::PartialWrite,
                                     Operation::Flush, Operation::Sync, Operation::Close, Operation::Rename}) {
            testPersistentSnapshotFailureStopsGrowth(operation);
        }
        return 0;
    }
    if (group == "--prune-growth") {
        testPersistentPruneFailureStopsGrowth();
        return 0;
    }
    if (group == "--missing-segment") {
        testMissingRetainedSegmentDoesNotOverPrune();
        return 0;
    }
    if (group == "--first-cause") {
        testConcurrentDegradationReportKeepsFirstCause();
        return 0;
    }
    if (group == "--final-close") {
        testFinalCloseFailureIsReported();
        return 0;
    }
    if (group == "--snapshot-close-proof") {
        testSnapshotCloseObservationProof();
        return 0;
    }
    if (group == "--blocked-stop-proof") {
        testBlockedStopOwnershipProof();
        return 0;
    }
    if (group == "--startup-artifacts") {
        testStartupClearsReplayGuardAfterArtifactPurge();
        testStartupCleanupFailureKeepsReplayGuard();
        testStartupGuardClearFailureDoesNotPublishSegment();
        return 0;
    }
    if (group == "--guard-clear-failure") {
        testStartupGuardClearFailureDoesNotPublishSegment();
        return 0;
    }
    if (group == "all" || group == "--durability") {
        testCompletedOrdering();
        testRollingPublication();
        for (const auto operation :
             {Operation::Write, Operation::PartialWrite, Operation::Flush, Operation::Sync, Operation::Close}) {
            for (int occurrence = 1; occurrence <= 3; ++occurrence) {
                testRotationFault(operation, occurrence);
            }
        }
        testRotationFault(Operation::Rename, 1);
        testRotationFault(Operation::Rename, 2);
        testRotationFault(Operation::Reopen, 1);
        testSnapshotDoesNotAcknowledgeActiveData();
        for (const auto operation : {Operation::Write, Operation::PartialWrite, Operation::Flush, Operation::Sync,
                                     Operation::Close, Operation::Rename}) {
            testPriorSnapshotSurvives(operation);
        }
        std::cout << "P1#4 prior-good snapshot and retained segment preservation, six failures: PASS\n";
        for (const auto operation : {Operation::SnapshotOpen, Operation::Write, Operation::PartialWrite,
                                     Operation::Flush, Operation::Sync, Operation::Close, Operation::Rename}) {
            testPersistentSnapshotFailureStopsGrowth(operation);
        }
        testPersistentPruneFailureStopsGrowth();
        testMissingRetainedSegmentDoesNotOverPrune();
        testConcurrentDegradationReportKeepsFirstCause();
        testFinalCloseFailureIsReported();
        testStartupClearsReplayGuardAfterArtifactPurge();
        testStartupCleanupFailureKeepsReplayGuard();
        testStartupGuardClearFailureDoesNotPublishSegment();
        for (const auto *point :
             {"durable-record", "before-header", "partial-header", "before-publish", "after-publish"}) {
            testCrash(argv[0], point);
        }
        std::cout << "P1#4 durability, rotation faults and subprocess crashes: PASS\n";
    }
    if (group == "all" || group == "--flush") {
        testRunningFlush(true);
        testRunningFlush(false);
        testRequestDuringSync();
        testFlushCoversBacklog();
        testUnpublishedReservationDoesNotResync();
        testRequestAtWaitBoundary();
        testQueueCannotStarveSync();
        testFailedSyncState(Operation::Flush);
        testFailedSyncState(Operation::Sync);
        std::cout << "P2#15 running explicit/idle/concurrent-request flush and failure state: PASS\n";
    }
}
