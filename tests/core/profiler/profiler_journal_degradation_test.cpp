#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "core/profiler/profiler.h"
#include "core/recovery/recovery_writer.h"
#include "native/sampler/types.h"

namespace spark {

struct ProfilerLifecycleTestAccess {
    static void installRecoveryWriter(Profiler &profiler, std::unique_ptr<RecoveryWriter> writer)
    {
        std::scoped_lock lock(profiler.recovery_mutex_);
        profiler.recovery_writer_ = std::move(writer);
        profiler.journal_degradation_logged_.store(false, std::memory_order_release);
    }

    static void setJournalDegradationActive(Profiler &profiler, bool active)
    {
        profiler.journal_degradation_active_.store(active, std::memory_order_release);
    }

    static void stopRecoveryWriter(Profiler &profiler) { profiler.stopRecoveryWriter(); }
    static bool reapRecoveryWriter(Profiler &profiler) { return profiler.reapRecoveryWriter(); }
};

}  // namespace spark

namespace {

using namespace std::chrono_literals;
using spark::RecoveryWriter;

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

std::filesystem::path testRoot(std::string_view name)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("spark_profiler_journal_degradation_" + std::string(name));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    return root;
}

// Fails exactly one selected journal operation once armed. Shared ownership keeps the hook valid.
class IoFailureGate {
public:
    explicit IoFailureGate(RecoveryWriter::IoOperation operation) : operation_(operation) {}

    void arm() { armed_.store(true, std::memory_order_release); }

    bool hook(RecoveryWriter::IoOperation operation)
    {
        if (operation != operation_) {
            return true;
        }
        bool expected = true;
        if (!armed_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return true;
        }
        tripped_.store(true, std::memory_order_release);
        return false;
    }

    bool tripped() const { return tripped_.load(std::memory_order_acquire); }

private:
    RecoveryWriter::IoOperation operation_;
    std::atomic<bool> armed_{false};
    std::atomic<bool> tripped_{false};
};

struct Fixture {
    std::filesystem::path root;
    std::shared_ptr<IoFailureGate> gate;
    std::unique_ptr<RecoveryWriter> writer;
    bool started = false;
};

Fixture makeFixture(std::string_view name,
                    RecoveryWriter::IoOperation failure_operation = RecoveryWriter::IoOperation::Write)
{
    Fixture fixture;
    fixture.root = testRoot(name);
    fixture.gate = std::make_shared<IoFailureGate>(failure_operation);
    RecoveryWriter::Config config;
    config.directory = fixture.root / "session";
    config.session_id = 7;
    config.flush_interval_ms = 10;
    config.sync_interval_ms = 60'000;
    config.shutdown_timeout_ms = 2000;
    const auto gate = fixture.gate;
    config.io_hook = [gate](RecoveryWriter::IoOperation operation) {
        return gate->hook(operation);
    };
    fixture.writer = std::make_unique<RecoveryWriter>(std::move(config));
    fixture.started = fixture.writer->start();
    return fixture;
}

void journalRecord(RecoveryWriter &writer)
{
    spark::Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 1;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writer.journalSample(sample);
}

// (a) A healthy journal reports nothing, repeatedly.
void testHealthyWriterStaysQuiet()
{
    auto fixture = makeFixture("healthy");
    assert(fixture.started);
    spark::Profiler profiler;
    profiler.setRecoveryDirectory(fixture.root);
    auto *writer = fixture.writer.get();
    spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(fixture.writer));
    spark::ProfilerLifecycleTestAccess::setJournalDegradationActive(profiler, true);

    journalRecord(*writer);
    assert(waitFor([&] { return writer->writtenRecords() > 0; }, 2s));

    std::string cause;
    assert(!profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause.empty());
    assert(!profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause.empty());
}

// (b)+(c) Degradation surfaces once with the recorded cause, then stays quiet.
void testDegradationReportsOnceWithCause()
{
    auto fixture = makeFixture("degraded");
    assert(fixture.started);
    spark::Profiler profiler;
    profiler.setRecoveryDirectory(fixture.root);
    auto *writer = fixture.writer.get();
    spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(fixture.writer));
    spark::ProfilerLifecycleTestAccess::setJournalDegradationActive(profiler, true);

    fixture.gate->arm();
    journalRecord(*writer);
    assert(waitFor([&] { return writer->journalDegraded(); }, 2s));
    assert(fixture.gate->tripped());

    std::string cause;
    assert(profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause == "write_failed");

    std::string repeat_cause;
    assert(!profiler.reportJournalDegradationIfNeeded(repeat_cause));
    assert(repeat_cause.empty());
    spark::ProfilerLifecycleTestAccess::stopRecoveryWriter(profiler);
    std::string retired_cause;
    assert(!profiler.reportJournalDegradationIfNeeded(retired_cause));
    assert(retired_cause.empty());
}

// A cold pre-flag short-circuits without inspecting the writer; arming it surfaces the cause.
void testInactivePreFlagShortCircuits()
{
    auto fixture = makeFixture("inactive");
    assert(fixture.started);
    spark::Profiler profiler;
    profiler.setRecoveryDirectory(fixture.root);
    auto *writer = fixture.writer.get();
    spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(fixture.writer));

    fixture.gate->arm();
    journalRecord(*writer);
    assert(waitFor([&] { return writer->journalDegraded(); }, 2s));

    std::string cause;
    assert(!profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause.empty());

    spark::ProfilerLifecycleTestAccess::setJournalDegradationActive(profiler, true);
    assert(profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause == "write_failed");
}

// The final flush can degrade after the last live poll, but stop must preserve its cause.
void testFinalSyncFailureSurvivesStop()
{
    auto fixture = makeFixture("final_sync", RecoveryWriter::IoOperation::Sync);
    assert(fixture.started);
    spark::Profiler profiler;
    profiler.setRecoveryDirectory(fixture.root);
    auto *writer = fixture.writer.get();
    spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(fixture.writer));
    spark::ProfilerLifecycleTestAccess::setJournalDegradationActive(profiler, true);

    journalRecord(*writer);
    assert(waitFor([&] { return writer->writtenRecords() > 0; }, 2s));
    std::string cause;
    assert(!profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause.empty());

    fixture.gate->arm();
    spark::ProfilerLifecycleTestAccess::stopRecoveryWriter(profiler);
    assert(fixture.gate->tripped());

    assert(profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause == "sync_failed");
    std::string repeat_cause;
    assert(!profiler.reportJournalDegradationIfNeeded(repeat_cause));
    assert(repeat_cause.empty());
}

void testUnpolledWriteFailureSurvivesStop()
{
    auto fixture = makeFixture("unpolled_stop");
    assert(fixture.started);
    spark::Profiler profiler;
    profiler.setRecoveryDirectory(fixture.root);
    auto *writer = fixture.writer.get();
    spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(fixture.writer));
    spark::ProfilerLifecycleTestAccess::setJournalDegradationActive(profiler, true);

    fixture.gate->arm();
    journalRecord(*writer);
    assert(waitFor([&] { return writer->journalDegraded(); }, 2s));
    assert(fixture.gate->tripped());
    spark::ProfilerLifecycleTestAccess::stopRecoveryWriter(profiler);

    std::string cause;
    assert(profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause == "write_failed");
}

void testUnpolledWriteFailureSurvivesReap()
{
    auto fixture = makeFixture("unpolled_reap");
    assert(fixture.started);
    spark::Profiler profiler;
    profiler.setRecoveryDirectory(fixture.root);
    auto *writer = fixture.writer.get();
    spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(fixture.writer));
    spark::ProfilerLifecycleTestAccess::setJournalDegradationActive(profiler, true);

    fixture.gate->arm();
    journalRecord(*writer);
    assert(waitFor([&] { return writer->journalDegraded(); }, 2s));
    writer->requestStop();
    assert(waitFor([&] { return writer->workerExited(); }, 2s));
    assert(spark::ProfilerLifecycleTestAccess::reapRecoveryWriter(profiler));

    std::string cause;
    assert(profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause == "write_failed");
}

void testPendingNoticeSurvivesSessionRestart()
{
    auto first = makeFixture("restart_first");
    assert(first.started);
    spark::Profiler profiler;
    profiler.setRecoveryDirectory(first.root);
    auto *first_writer = first.writer.get();
    spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(first.writer));
    spark::ProfilerLifecycleTestAccess::setJournalDegradationActive(profiler, true);

    first.gate->arm();
    journalRecord(*first_writer);
    assert(waitFor([&] { return first_writer->journalDegraded(); }, 2s));
    spark::ProfilerLifecycleTestAccess::stopRecoveryWriter(profiler);

    auto second = makeFixture("restart_second", RecoveryWriter::IoOperation::Sync);
    assert(second.started);
    auto *second_writer = second.writer.get();
    spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(second.writer));
    spark::ProfilerLifecycleTestAccess::setJournalDegradationActive(profiler, true);
    journalRecord(*second_writer);
    assert(waitFor([&] { return second_writer->writtenRecords() > 0; }, 2s));
    second.gate->arm();
    second_writer->requestFlush();
    assert(waitFor([&] { return second_writer->journalDegraded(); }, 2s));

    std::string cause;
    assert(profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause == "write_failed");
    assert(profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause == "sync_failed");
    std::string repeat_cause;
    assert(!profiler.reportJournalDegradationIfNeeded(repeat_cause));
    assert(repeat_cause.empty());
    spark::ProfilerLifecycleTestAccess::stopRecoveryWriter(profiler);
}

void testHealthyRetirementIsQuiet()
{
    auto fixture = makeFixture("healthy_retirement");
    assert(fixture.started);
    spark::Profiler profiler;
    profiler.setRecoveryDirectory(fixture.root);
    spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(fixture.writer));
    spark::ProfilerLifecycleTestAccess::setJournalDegradationActive(profiler, true);
    spark::ProfilerLifecycleTestAccess::stopRecoveryWriter(profiler);

    std::string cause;
    assert(!profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause.empty());
}

void testOverflowCoalescesAfterEightPendingCauses()
{
    spark::Profiler profiler;
    for (int i = 0; i < 10; ++i) {
        auto fixture = makeFixture("overflow_" + std::to_string(i));
        assert(fixture.started);
        auto *writer = fixture.writer.get();
        spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(fixture.writer));
        spark::ProfilerLifecycleTestAccess::setJournalDegradationActive(profiler, true);
        fixture.gate->arm();
        journalRecord(*writer);
        assert(waitFor([&] { return writer->journalDegraded(); }, 2s));
        spark::ProfilerLifecycleTestAccess::stopRecoveryWriter(profiler);
    }

    std::string cause;
    for (int i = 0; i < 8; ++i) {
        assert(profiler.reportJournalDegradationIfNeeded(cause));
        assert(cause == "write_failed");
    }
    assert(profiler.reportJournalDegradationIfNeeded(cause));
    assert(cause == "additional recovery journals degraded");
    assert(!profiler.reportJournalDegradationIfNeeded(cause));
}

}  // namespace

int main()
{
    testHealthyWriterStaysQuiet();
    testDegradationReportsOnceWithCause();
    testInactivePreFlagShortCircuits();
    testFinalSyncFailureSurvivesStop();
    testUnpolledWriteFailureSurvivesStop();
    testUnpolledWriteFailureSurvivesReap();
    testPendingNoticeSurvivesSessionRestart();
    testHealthyRetirementIsQuiet();
    testOverflowCoalescesAfterEightPendingCauses();
    std::cout << "Profiler journal degradation tests passed.\n";
    return 0;
}
