#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32) && defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "native/alloc/allocation_diagnostics_test_access.h"
#include "native/alloc/allocation_lifecycle_test_access.h"
#include "native/alloc/allocation_sampler.h"
#include "native/sampler/thread_info.h"

namespace {

using Clock = std::chrono::steady_clock;

bool report(const char *message)
{
    std::fprintf(stderr, "allocation diagnostics: %s\n", message);
    return false;
}

template <typename Predicate>
bool waitFor(Predicate &&predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

spark::AllocationSamplerConfig makeConfig(std::uint64_t seed)
{
    spark::AllocationSamplerConfig config;
    config.interval_bytes = 1;
    config.session_seed = seed;
    return config;
}

spark::AllocationSamplerConfig makeFixtureConfig(std::uint64_t seed)
{
    spark::AllocationSamplerConfig config = makeConfig(seed);
    config.only_ticks_over_ms = 1;
    return config;
}

struct FixtureCleanupObservation {
    bool cleanup_ok = false;
    bool backing_was_valid = false;
    bool worker_gate_released = false;
    bool worker_gate_timed_out = false;
};

struct DiagnosticsFixture {
    static constexpr unsigned char KBackingCookie = 0xa5;

    spark::test::AllocationFixtureWorkerGate worker_gate;
    spark::test::AllocationEventProcessingGate event_gate;
    spark::test::AllocationFixtureLockGate lock_gate;
    std::array<unsigned char, 4096> backing{};
    std::mutex helper_mutex;
    std::exception_ptr helper_exception;
    std::string stop_error;
    std::string start_error;
    std::string lock_error;
    std::thread stop_helper;
    std::thread start_helper;
    std::thread lock_helper;
    std::atomic<bool> stop_done{false};
    std::atomic<bool> start_done{false};
    std::atomic<bool> lock_done{false};
    bool stop_ok = false;
    bool start_ok = false;
    bool lock_ok = false;
    spark::test::StartFailureGate start_failure_gate;
    spark::test::AllocationFixtureCpuWorkControl cpu_work;
    spark::AllocationSampler sampler;
    FixtureCleanupObservation *observation = nullptr;
    bool configured = false;
    bool worker_configured = false;
    bool cleaned = false;
    bool cleanup_result = false;

    explicit DiagnosticsFixture(FixtureCleanupObservation *cleanup_observation = nullptr,
                                std::uint32_t cpu_sleep_ms = 0)
        : cpu_work(cpu_sleep_ms), observation(cleanup_observation)
    {
        backing.fill(KBackingCookie);
    }

    ~DiagnosticsFixture() noexcept { cleanup(); }

    bool configure(bool no_hooks, bool no_worker, spark::test::AllocationFixtureWorkerGate *gate = nullptr)
    {
        if (configured ||
            !spark::test::AllocationDiagnosticsTestAccess::configureFixture(sampler, no_hooks, no_worker, gate)) {
            return false;
        }
        configured = true;
        worker_configured = !no_worker;
        return true;
    }

    bool configureCpuWork()
    {
        return spark::test::AllocationDiagnosticsTestAccess::configureFixtureCpuWork(sampler, cpu_work);
    }

    void recordHelperFailure() noexcept
    {
        try {
            std::scoped_lock lock(helper_mutex);
            if (helper_exception == nullptr) {
                helper_exception = std::current_exception();
            }
        }
        catch (...) {
            return;
        }
    }

    bool helperFailed() noexcept
    {
        try {
            std::scoped_lock lock(helper_mutex);
            return helper_exception != nullptr;
        }
        catch (...) {
            return true;
        }
    }

    void launchStopHelper()
    {
        stop_done.store(false, std::memory_order_release);
        try {
            stop_helper = std::thread([this] {
                try {
                    stop_ok = sampler.stop(stop_error);
                }
                catch (...) {
                    stop_ok = false;
                    recordHelperFailure();
                }
                stop_done.store(true, std::memory_order_release);
            });
        }
        catch (...) {
            recordHelperFailure();
            stop_done.store(true, std::memory_order_release);
        }
    }

    void launchStartHelper(const spark::AllocationSamplerConfig &config)
    {
        start_done.store(false, std::memory_order_release);
        try {
            start_helper = std::thread([this, config] {
                try {
                    start_ok = sampler.start(config, start_error);
                }
                catch (...) {
                    start_ok = false;
                    recordHelperFailure();
                }
                start_done.store(true, std::memory_order_release);
            });
        }
        catch (...) {
            recordHelperFailure();
            start_done.store(true, std::memory_order_release);
        }
    }

    void launchLockHelper(void *pointer, bool insertion)
    {
        lock_done.store(false, std::memory_order_release);
        lock_ok = false;
        lock_gate.ready.store(false, std::memory_order_release);
        lock_gate.release.store(false, std::memory_order_release);
        lock_gate.timed_out.store(false, std::memory_order_release);
        try {
            lock_helper = std::thread([this, pointer, insertion] {
                try {
                    lock_ok = insertion
                                ? spark::test::AllocationDiagnosticsTestAccess::holdInsertionShardLock(sampler, pointer,
                                                                                                       lock_gate)
                                : spark::test::AllocationDiagnosticsTestAccess::holdDetachShardLock(sampler, pointer,
                                                                                                    lock_gate);
                }
                catch (...) {
                    lock_ok = false;
                    recordHelperFailure();
                }
                lock_done.store(true, std::memory_order_release);
            });
        }
        catch (...) {
            recordHelperFailure();
            lock_done.store(true, std::memory_order_release);
        }
    }

    void joinHelpers() noexcept
    {
        if (stop_helper.joinable()) {
            (void)waitFor([&] { return stop_done.load(std::memory_order_acquire); }, std::chrono::seconds(8));
            stop_helper.join();
        }
        if (start_helper.joinable()) {
            (void)waitFor([&] { return start_done.load(std::memory_order_acquire); }, std::chrono::seconds(8));
            start_helper.join();
        }
        if (lock_helper.joinable()) {
            (void)waitFor([&] { return lock_done.load(std::memory_order_acquire); }, std::chrono::seconds(8));
            lock_helper.join();
        }
    }

    bool cleanup() noexcept
    {
        if (cleaned) {
            return cleanup_result;
        }

        if (configured) {
            if (!worker_configured) {
                spark::test::AllocationDiagnosticsTestAccess::forceProcessEventFailure(sampler, false);
            }
            cpu_work.request_cancel.store(true, std::memory_order_release);
            start_failure_gate.fail_now.store(true, std::memory_order_release);
            sampler.requestStop();
        }
        worker_gate.release.store(true, std::memory_order_release);
        event_gate.release.store(true, std::memory_order_release);
        lock_gate.release.store(true, std::memory_order_release);
        joinHelpers();
        if (configured) {
            spark::test::AllocationLifecycleTestAccess::disarmThreadCreationFailure(sampler);
        }

        bool shutdown_ok = true;
        bool release_ok = !configured;
        try {
            std::string error;
            shutdown_ok = sampler.shutdown(error);
            if (configured && shutdown_ok) {
                spark::test::AllocationDiagnosticsTestAccess::forceProcessEventFailure(sampler, false);
                spark::test::AllocationDiagnosticsTestAccess::forceAggregatorCpuReadFailure(sampler, false);
                spark::test::AllocationDiagnosticsTestAccess::forceAggregatorCpuZero(sampler, false);
                release_ok = spark::test::AllocationDiagnosticsTestAccess::releaseFixture(sampler);
            }
        }
        catch (...) {
            shutdown_ok = false;
            recordHelperFailure();
        }
        cleanup_result = shutdown_ok && release_ok && !helperFailed();
        cleaned = true;

        if (observation != nullptr) {
            observation->cleanup_ok = cleanup_result;
            observation->backing_was_valid = backing[0] == KBackingCookie;
            observation->worker_gate_released = worker_gate.release.load(std::memory_order_acquire);
            observation->worker_gate_timed_out = worker_gate.timed_out.load(std::memory_order_acquire);
        }
        return cleanup_result;
    }
};

bool supportedBackend();

bool diagnosticsCountersZero(const spark::AllocationDiagnostics &diagnostics)
{
    return diagnostics.drain_truncated == 0 && diagnostics.drain_truncated_allocation_events == 0 &&
           diagnostics.drain_truncated_thread_observation_events == 0 && diagnostics.drain_truncated_tick_events == 0 &&
           diagnostics.retained_allocations_skipped == 0 && diagnostics.record_pool_acquisition_failures == 0 &&
           diagnostics.insertion_contention_failures == 0 && diagnostics.exhausted_insertion_probe_failures == 0 &&
           diagnostics.detach_contention_attempts == 0 && diagnostics.processed_allocation_events == 0 &&
           diagnostics.processed_thread_observation_events == 0 && diagnostics.processed_tick_events == 0 &&
           diagnostics.discarded_allocation_events == 0 && diagnostics.discarded_thread_observation_events == 0 &&
           diagnostics.discarded_tick_events == 0 && diagnostics.consumer_lifetime_elapsed_ns == 0 &&
           diagnostics.active_drain_elapsed_ns == 0 && diagnostics.caller_final_drain_elapsed_ns == 0 &&
           diagnostics.caller_final_drain_allocation_events == 0 &&
           diagnostics.caller_final_drain_thread_observation_events == 0 &&
           diagnostics.caller_final_drain_tick_events == 0 && !diagnostics.aggregator_cpu_valid &&
           !diagnostics.aggregator_cpu_read_failure && diagnostics.aggregator_cpu_time_ns == 0 &&
           diagnostics.module_cache_hits == 0 && diagnostics.module_cache_misses == 0 &&
           diagnostics.module_cache_insertion_refusals == 0 && diagnostics.module_cache_size == 0;
}

bool diagnosticsEqual(const spark::AllocationDiagnostics &left, const spark::AllocationDiagnostics &right)
{
    return left.supported == right.supported && left.accounting_state == right.accounting_state &&
           left.live_index_capacity == right.live_index_capacity &&
           left.live_record_capacity == right.live_record_capacity && left.drain_truncated == right.drain_truncated &&
           left.drain_truncated_allocation_events == right.drain_truncated_allocation_events &&
           left.drain_truncated_thread_observation_events == right.drain_truncated_thread_observation_events &&
           left.drain_truncated_tick_events == right.drain_truncated_tick_events &&
           left.retained_allocations_skipped == right.retained_allocations_skipped &&
           left.record_pool_acquisition_failures == right.record_pool_acquisition_failures &&
           left.insertion_contention_failures == right.insertion_contention_failures &&
           left.exhausted_insertion_probe_failures == right.exhausted_insertion_probe_failures &&
           left.detach_contention_attempts == right.detach_contention_attempts &&
           left.processed_allocation_events == right.processed_allocation_events &&
           left.processed_thread_observation_events == right.processed_thread_observation_events &&
           left.processed_tick_events == right.processed_tick_events &&
           left.discarded_allocation_events == right.discarded_allocation_events &&
           left.discarded_thread_observation_events == right.discarded_thread_observation_events &&
           left.discarded_tick_events == right.discarded_tick_events &&
           left.consumer_lifetime_elapsed_ns == right.consumer_lifetime_elapsed_ns &&
           left.active_drain_elapsed_ns == right.active_drain_elapsed_ns &&
           left.caller_final_drain_elapsed_ns == right.caller_final_drain_elapsed_ns &&
           left.caller_final_drain_allocation_events == right.caller_final_drain_allocation_events &&
           left.caller_final_drain_thread_observation_events == right.caller_final_drain_thread_observation_events &&
           left.caller_final_drain_tick_events == right.caller_final_drain_tick_events &&
           left.aggregator_cpu_supported == right.aggregator_cpu_supported &&
           left.aggregator_cpu_valid == right.aggregator_cpu_valid &&
           left.aggregator_cpu_read_failure == right.aggregator_cpu_read_failure &&
           left.aggregator_cpu_time_ns == right.aggregator_cpu_time_ns &&
           left.module_cache_supported == right.module_cache_supported &&
           left.module_cache_hits == right.module_cache_hits && left.module_cache_misses == right.module_cache_misses &&
           left.module_cache_insertion_refusals == right.module_cache_insertion_refusals &&
           left.module_cache_size == right.module_cache_size;
}

bool verifyFixtureOrdinary()
{
    if (!supportedBackend()) {
        return true;
    }

    DiagnosticsFixture fixture;
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(makeFixtureConfig(0x2001), error)) {
        return report("ordinary fixture start failed");
    }
    const spark::AllocationDiagnostics diagnostics = fixture.sampler.diagnostics();
    const bool valid = diagnostics.accounting_state == spark::AllocationAccountingState::Active &&
                       spark::test::AllocationDiagnosticsTestAccess::fixtureStorageReady(fixture.sampler) &&
                       !spark::test::AllocationDiagnosticsTestAccess::fixtureHooksPresent(fixture.sampler) &&
                       !spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler);
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && valid ? true : report("ordinary fixture oracle failed");
}

bool verifyFixtureSnapshot()
{
    if (!supportedBackend()) {
        return true;
    }

    DiagnosticsFixture fixture;
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(makeFixtureConfig(0x2002), error)) {
        return report("fixture snapshot start failed");
    }
    spark::test::AllocationFixtureSeedCounts seeded;
    const bool seed_ok = spark::test::AllocationDiagnosticsTestAccess::seedFixtureQueues(fixture.sampler, seeded);
    const bool worker_absent = !spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler);
    const spark::AllocationDiagnostics before = fixture.sampler.diagnostics();
    spark::AllocationSnapshot snapshot;
    const bool captured = seed_ok && fixture.sampler.snapshot(snapshot, error);
    const spark::AllocationDiagnostics after = fixture.sampler.diagnostics();
    const auto exact_delta = [](std::uint64_t after_value, std::uint64_t before_value, std::uint64_t expected) {
        return after_value >= before_value && after_value - before_value == expected;
    };
    const bool processed_exact =
        exact_delta(after.processed_allocation_events, before.processed_allocation_events, 3) &&
        exact_delta(after.processed_thread_observation_events, before.processed_thread_observation_events, 2) &&
        exact_delta(after.processed_tick_events, before.processed_tick_events, 5);
    const bool discarded_zero =
        after.discarded_allocation_events == before.discarded_allocation_events &&
        after.discarded_thread_observation_events == before.discarded_thread_observation_events &&
        after.discarded_tick_events == before.discarded_tick_events &&
        after.drain_truncated == before.drain_truncated &&
        after.drain_truncated_allocation_events == before.drain_truncated_allocation_events &&
        after.drain_truncated_thread_observation_events == before.drain_truncated_thread_observation_events &&
        after.drain_truncated_tick_events == before.drain_truncated_tick_events;
    const bool final_event_categories_unchanged =
        after.caller_final_drain_allocation_events == before.caller_final_drain_allocation_events &&
        after.caller_final_drain_thread_observation_events == before.caller_final_drain_thread_observation_events &&
        after.caller_final_drain_tick_events == before.caller_final_drain_tick_events;
    const bool timing_categories_unchanged =
        after.consumer_lifetime_elapsed_ns == before.consumer_lifetime_elapsed_ns &&
        after.active_drain_elapsed_ns == before.active_drain_elapsed_ns &&
        after.caller_final_drain_elapsed_ns == before.caller_final_drain_elapsed_ns;
    const bool valid = worker_absent && seeded.allocation_events == 3 && seeded.thread_observation_events == 2 &&
                       seeded.tick_events == 5 && captured && processed_exact && discarded_zero &&
                       final_event_categories_unchanged && timing_categories_unchanged;
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && valid ? true : report("fixture snapshot oracle failed");
}

bool verifyFixtureReset()
{
    if (!supportedBackend()) {
        return true;
    }

    DiagnosticsFixture fixture;
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(makeFixtureConfig(0x2003), error)) {
        return report("fixture reset start failed");
    }
    const bool recorded = spark::test::AllocationDiagnosticsTestAccess::recordFixtureAllocation(
        fixture.sampler, fixture.backing.data(), fixture.backing.size());
    const bool first_input = recorded && fixture.sampler.enqueuedSamples() > 0;
    const bool first_stop = fixture.sampler.stop(error);
    const spark::AllocationDiagnostics first_terminal = fixture.sampler.diagnostics();
    const bool restarted = fixture.sampler.start(makeFixtureConfig(0x2004), error);
    const spark::AllocationDiagnostics reset = fixture.sampler.diagnostics();
    const bool reset_valid = restarted && reset.accounting_state == spark::AllocationAccountingState::Active &&
                             diagnosticsCountersZero(reset) && fixture.sampler.enqueuedSamples() == 0;
    const bool second_stop = restarted && fixture.sampler.stop(error);
    const spark::AllocationDiagnostics second_terminal = fixture.sampler.diagnostics();
    const bool valid = first_input && first_stop &&
                       first_terminal.accounting_state == spark::AllocationAccountingState::Complete && reset_valid &&
                       second_stop && second_terminal.accounting_state == spark::AllocationAccountingState::Complete;
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && valid ? true : report("fixture reset oracle failed");
}

bool verifyFixtureDirectRecord()
{
    if (!supportedBackend()) {
        return true;
    }

    DiagnosticsFixture fixture;
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(makeFixtureConfig(0x2005), error)) {
        return report("fixture direct-record start failed");
    }
    const std::uint64_t before = fixture.sampler.enqueuedSamples();
    const bool recorded = spark::test::AllocationDiagnosticsTestAccess::recordFixtureAllocation(
        fixture.sampler, fixture.backing.data(), fixture.backing.size());
    const bool positive_enqueue = fixture.sampler.enqueuedSamples() > before;
    const bool stopped = fixture.sampler.stop(error);
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && recorded && positive_enqueue && stopped ? true : report("fixture direct-record oracle failed");
}

bool exactDelta(std::uint64_t after, std::uint64_t before, std::uint64_t expected)
{
    return after >= before && after - before == expected;
}

bool verifyFixtureWindowsBoundedAccounting()
{
#if !defined(_WIN32) || !defined(_M_X64)
    return true;
#else
    DiagnosticsFixture fixture;
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(makeFixtureConfig(0x2101), error)) {
        return report("fixture bounded-accounting start failed");
    }
    spark::test::AllocationFixtureSeedCounts seeded;
    const bool seeded_ok = spark::test::AllocationDiagnosticsTestAccess::seedFixtureQueues(fixture.sampler, seeded);
    const spark::AllocationDiagnostics before = fixture.sampler.diagnostics();
    spark::test::AllocationDiagnosticsTestAccess::forceDrainDeadline(fixture.sampler, true);
    spark::AllocationSnapshot snapshot;
    const bool captured = seeded_ok && fixture.sampler.snapshot(snapshot, error);
    const std::string snapshot_error = error;
    const spark::AllocationDiagnostics after_snapshot = fixture.sampler.diagnostics();
    const bool snapshot_valid =
        seeded.allocation_events == 3 && seeded.thread_observation_events == 2 && seeded.tick_events == 5 && captured &&
        exactDelta(after_snapshot.drain_truncated, before.drain_truncated, 6) &&
        exactDelta(after_snapshot.drain_truncated_allocation_events, before.drain_truncated_allocation_events, 3) &&
        exactDelta(after_snapshot.drain_truncated_thread_observation_events,
                   before.drain_truncated_thread_observation_events, 2) &&
        exactDelta(after_snapshot.drain_truncated_tick_events, before.drain_truncated_tick_events, 1) &&
        exactDelta(after_snapshot.discarded_allocation_events, before.discarded_allocation_events, 3) &&
        exactDelta(after_snapshot.discarded_thread_observation_events, before.discarded_thread_observation_events, 2) &&
        exactDelta(after_snapshot.discarded_tick_events, before.discarded_tick_events, 1) &&
        after_snapshot.processed_allocation_events == before.processed_allocation_events &&
        after_snapshot.processed_thread_observation_events == before.processed_thread_observation_events &&
        after_snapshot.processed_tick_events == before.processed_tick_events &&
        after_snapshot.caller_final_drain_allocation_events == before.caller_final_drain_allocation_events &&
        after_snapshot.caller_final_drain_thread_observation_events ==
            before.caller_final_drain_thread_observation_events &&
        after_snapshot.caller_final_drain_tick_events == before.caller_final_drain_tick_events &&
        after_snapshot.consumer_lifetime_elapsed_ns == before.consumer_lifetime_elapsed_ns &&
        after_snapshot.active_drain_elapsed_ns == before.active_drain_elapsed_ns &&
        after_snapshot.caller_final_drain_elapsed_ns == before.caller_final_drain_elapsed_ns;

    const bool stopped = fixture.sampler.stop(error);
    const std::string stop_error = error;
    const spark::AllocationDiagnostics terminal = fixture.sampler.diagnostics();
    spark::test::AllocationDiagnosticsTestAccess::forceDrainDeadline(fixture.sampler, false);
    const bool stop_valid =
        stopped && terminal.accounting_state == spark::AllocationAccountingState::Complete &&
        exactDelta(terminal.discarded_tick_events, after_snapshot.discarded_tick_events, 4) &&
        exactDelta(terminal.drain_truncated_tick_events, after_snapshot.drain_truncated_tick_events, 4) &&
        exactDelta(terminal.drain_truncated, after_snapshot.drain_truncated, 4) &&
        exactDelta(terminal.caller_final_drain_tick_events, after_snapshot.caller_final_drain_tick_events, 4) &&
        terminal.discarded_allocation_events == before.discarded_allocation_events + 3 &&
        terminal.discarded_thread_observation_events == before.discarded_thread_observation_events + 2 &&
        terminal.discarded_tick_events == before.discarded_tick_events + 5 &&
        terminal.drain_truncated_allocation_events == before.drain_truncated_allocation_events + 3 &&
        terminal.drain_truncated_thread_observation_events == before.drain_truncated_thread_observation_events + 2 &&
        terminal.drain_truncated_tick_events == before.drain_truncated_tick_events + 5 &&
        terminal.drain_truncated == before.drain_truncated + 10 &&
        terminal.caller_final_drain_allocation_events == before.caller_final_drain_allocation_events &&
        terminal.caller_final_drain_thread_observation_events == before.caller_final_drain_thread_observation_events &&
        terminal.caller_final_drain_tick_events == before.caller_final_drain_tick_events + 4 &&
        terminal.caller_final_drain_elapsed_ns >= after_snapshot.caller_final_drain_elapsed_ns &&
        terminal.processed_allocation_events == after_snapshot.processed_allocation_events &&
        terminal.processed_thread_observation_events == after_snapshot.processed_thread_observation_events &&
        terminal.processed_tick_events == after_snapshot.processed_tick_events;
    const bool cleanup_ok = fixture.cleanup();
    if (!cleanup_ok || !snapshot_valid || !stop_valid) {
        std::fprintf(stderr,
                     "bounded-accounting: seeded_ok=%d captured=%d stopped=%d cleanup_ok=%d snapshot_valid=%d "
                     "stop_valid=%d snapshot_error='%s' stop_error='%s'\n",
                     static_cast<int>(seeded_ok), static_cast<int>(captured), static_cast<int>(stopped),
                     static_cast<int>(cleanup_ok), static_cast<int>(snapshot_valid), static_cast<int>(stop_valid),
                     snapshot_error.c_str(), stop_error.c_str());
        const auto predicate = [](const char *name, bool valid) {
            std::fprintf(stderr, "bounded-accounting predicate: %s=%d\n", name, static_cast<int>(valid));
        };
        predicate("snapshot_valid[0]", seeded.allocation_events == 3);
        predicate("snapshot_valid[1]", seeded.thread_observation_events == 2);
        predicate("snapshot_valid[2]", seeded.tick_events == 5);
        predicate("snapshot_valid[3]", captured);
        predicate("snapshot_valid[4]", exactDelta(after_snapshot.drain_truncated, before.drain_truncated, 6));
        predicate("snapshot_valid[5]", exactDelta(after_snapshot.drain_truncated_allocation_events,
                                                  before.drain_truncated_allocation_events, 3));
        predicate("snapshot_valid[6]", exactDelta(after_snapshot.drain_truncated_thread_observation_events,
                                                  before.drain_truncated_thread_observation_events, 2));
        predicate("snapshot_valid[7]",
                  exactDelta(after_snapshot.drain_truncated_tick_events, before.drain_truncated_tick_events, 1));
        predicate("snapshot_valid[8]",
                  exactDelta(after_snapshot.discarded_allocation_events, before.discarded_allocation_events, 3));
        predicate("snapshot_valid[9]", exactDelta(after_snapshot.discarded_thread_observation_events,
                                                  before.discarded_thread_observation_events, 2));
        predicate("snapshot_valid[10]",
                  exactDelta(after_snapshot.discarded_tick_events, before.discarded_tick_events, 1));
        predicate("snapshot_valid[11]",
                  after_snapshot.processed_allocation_events == before.processed_allocation_events);
        predicate("snapshot_valid[12]",
                  after_snapshot.processed_thread_observation_events == before.processed_thread_observation_events);
        predicate("snapshot_valid[13]", after_snapshot.processed_tick_events == before.processed_tick_events);
        predicate("snapshot_valid[14]",
                  after_snapshot.caller_final_drain_allocation_events == before.caller_final_drain_allocation_events);
        predicate("snapshot_valid[15]", after_snapshot.caller_final_drain_thread_observation_events ==
                                            before.caller_final_drain_thread_observation_events);
        predicate("snapshot_valid[16]",
                  after_snapshot.caller_final_drain_tick_events == before.caller_final_drain_tick_events);
        predicate("snapshot_valid[17]",
                  after_snapshot.consumer_lifetime_elapsed_ns == before.consumer_lifetime_elapsed_ns);
        predicate("snapshot_valid[18]", after_snapshot.active_drain_elapsed_ns == before.active_drain_elapsed_ns);
        predicate("snapshot_valid[19]",
                  after_snapshot.caller_final_drain_elapsed_ns == before.caller_final_drain_elapsed_ns);
        predicate("stop_valid[0]", stopped);
        predicate("stop_valid[1]", terminal.accounting_state == spark::AllocationAccountingState::Complete);
        predicate("stop_valid[2]", exactDelta(terminal.discarded_tick_events, after_snapshot.discarded_tick_events, 4));
        predicate("stop_valid[3]",
                  exactDelta(terminal.drain_truncated_tick_events, after_snapshot.drain_truncated_tick_events, 4));
        predicate("stop_valid[4]", exactDelta(terminal.drain_truncated, after_snapshot.drain_truncated, 4));
        predicate("stop_valid[5]", exactDelta(terminal.caller_final_drain_tick_events,
                                              after_snapshot.caller_final_drain_tick_events, 4));
        predicate("stop_valid[6]", terminal.discarded_allocation_events == before.discarded_allocation_events + 3);
        predicate("stop_valid[7]",
                  terminal.discarded_thread_observation_events == before.discarded_thread_observation_events + 2);
        predicate("stop_valid[8]", terminal.discarded_tick_events == before.discarded_tick_events + 5);
        predicate("stop_valid[9]",
                  terminal.drain_truncated_allocation_events == before.drain_truncated_allocation_events + 3);
        predicate("stop_valid[10]", terminal.drain_truncated_thread_observation_events ==
                                        before.drain_truncated_thread_observation_events + 2);
        predicate("stop_valid[11]", terminal.drain_truncated_tick_events == before.drain_truncated_tick_events + 5);
        predicate("stop_valid[12]", terminal.drain_truncated == before.drain_truncated + 10);
        predicate("stop_valid[13]",
                  terminal.caller_final_drain_allocation_events == before.caller_final_drain_allocation_events);
        predicate("stop_valid[14]", terminal.caller_final_drain_thread_observation_events ==
                                        before.caller_final_drain_thread_observation_events);
        predicate("stop_valid[15]",
                  terminal.caller_final_drain_tick_events == before.caller_final_drain_tick_events + 4);
        predicate("stop_valid[16]",
                  terminal.caller_final_drain_elapsed_ns >= after_snapshot.caller_final_drain_elapsed_ns);
        predicate("stop_valid[17]", terminal.processed_allocation_events == after_snapshot.processed_allocation_events);
        predicate("stop_valid[18]",
                  terminal.processed_thread_observation_events == after_snapshot.processed_thread_observation_events);
        predicate("stop_valid[19]", terminal.processed_tick_events == after_snapshot.processed_tick_events);
        const auto dump = [](const char *name, const spark::AllocationDiagnostics &value) {
            std::fprintf(stderr, "bounded-accounting %s.drain_truncated=%llu\n", name,
                         static_cast<unsigned long long>(value.drain_truncated));
            std::fprintf(stderr, "bounded-accounting %s.drain_truncated_allocation_events=%llu\n", name,
                         static_cast<unsigned long long>(value.drain_truncated_allocation_events));
            std::fprintf(stderr, "bounded-accounting %s.drain_truncated_thread_observation_events=%llu\n", name,
                         static_cast<unsigned long long>(value.drain_truncated_thread_observation_events));
            std::fprintf(stderr, "bounded-accounting %s.drain_truncated_tick_events=%llu\n", name,
                         static_cast<unsigned long long>(value.drain_truncated_tick_events));
            std::fprintf(stderr, "bounded-accounting %s.discarded_allocation_events=%llu\n", name,
                         static_cast<unsigned long long>(value.discarded_allocation_events));
            std::fprintf(stderr, "bounded-accounting %s.discarded_thread_observation_events=%llu\n", name,
                         static_cast<unsigned long long>(value.discarded_thread_observation_events));
            std::fprintf(stderr, "bounded-accounting %s.discarded_tick_events=%llu\n", name,
                         static_cast<unsigned long long>(value.discarded_tick_events));
            std::fprintf(stderr, "bounded-accounting %s.processed_allocation_events=%llu\n", name,
                         static_cast<unsigned long long>(value.processed_allocation_events));
            std::fprintf(stderr, "bounded-accounting %s.processed_thread_observation_events=%llu\n", name,
                         static_cast<unsigned long long>(value.processed_thread_observation_events));
            std::fprintf(stderr, "bounded-accounting %s.processed_tick_events=%llu\n", name,
                         static_cast<unsigned long long>(value.processed_tick_events));
            std::fprintf(stderr, "bounded-accounting %s.caller_final_drain_allocation_events=%llu\n", name,
                         static_cast<unsigned long long>(value.caller_final_drain_allocation_events));
            std::fprintf(stderr, "bounded-accounting %s.caller_final_drain_thread_observation_events=%llu\n", name,
                         static_cast<unsigned long long>(value.caller_final_drain_thread_observation_events));
            std::fprintf(stderr, "bounded-accounting %s.caller_final_drain_tick_events=%llu\n", name,
                         static_cast<unsigned long long>(value.caller_final_drain_tick_events));
            std::fprintf(stderr, "bounded-accounting %s.consumer_lifetime_elapsed_ns=%llu\n", name,
                         static_cast<unsigned long long>(value.consumer_lifetime_elapsed_ns));
            std::fprintf(stderr, "bounded-accounting %s.active_drain_elapsed_ns=%llu\n", name,
                         static_cast<unsigned long long>(value.active_drain_elapsed_ns));
            std::fprintf(stderr, "bounded-accounting %s.caller_final_drain_elapsed_ns=%llu\n", name,
                         static_cast<unsigned long long>(value.caller_final_drain_elapsed_ns));
            std::fprintf(stderr, "bounded-accounting %s.accounting_state=%llu\n", name,
                         static_cast<unsigned long long>(value.accounting_state));
        };
        dump("before", before);
        dump("after_snapshot", after_snapshot);
        dump("terminal", terminal);
    }
    return cleanup_ok && snapshot_valid && stop_valid ? true : report("fixture bounded-accounting oracle failed");
#endif
}

bool verifyFixtureLinuxProcessAll()
{
#if !defined(__linux__) || !defined(__x86_64__)
    return true;
#else
    DiagnosticsFixture fixture;
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(makeFixtureConfig(0x2102), error)) {
        return report("fixture Linux process-all start failed");
    }
    spark::test::AllocationFixtureSeedCounts seeded;
    const bool seeded_ok = spark::test::AllocationDiagnosticsTestAccess::seedFixtureQueues(fixture.sampler, seeded);
    const spark::AllocationDiagnostics before = fixture.sampler.diagnostics();
    spark::AllocationSnapshot snapshot;
    const bool captured = seeded_ok && fixture.sampler.snapshot(snapshot, error);
    const spark::AllocationDiagnostics after_snapshot = fixture.sampler.diagnostics();
    const bool snapshot_valid =
        seeded.allocation_events == 3 && seeded.thread_observation_events == 2 && seeded.tick_events == 5 && captured &&
        exactDelta(after_snapshot.processed_allocation_events, before.processed_allocation_events, 3) &&
        exactDelta(after_snapshot.processed_thread_observation_events, before.processed_thread_observation_events, 2) &&
        exactDelta(after_snapshot.processed_tick_events, before.processed_tick_events, 5) &&
        after_snapshot.discarded_allocation_events == before.discarded_allocation_events &&
        after_snapshot.discarded_thread_observation_events == before.discarded_thread_observation_events &&
        after_snapshot.discarded_tick_events == before.discarded_tick_events &&
        after_snapshot.drain_truncated == before.drain_truncated &&
        after_snapshot.drain_truncated_allocation_events == before.drain_truncated_allocation_events &&
        after_snapshot.drain_truncated_thread_observation_events == before.drain_truncated_thread_observation_events &&
        after_snapshot.drain_truncated_tick_events == before.drain_truncated_tick_events &&
        after_snapshot.caller_final_drain_allocation_events == before.caller_final_drain_allocation_events &&
        after_snapshot.caller_final_drain_thread_observation_events ==
            before.caller_final_drain_thread_observation_events &&
        after_snapshot.caller_final_drain_tick_events == before.caller_final_drain_tick_events;
    const bool stopped = fixture.sampler.stop(error);
    const spark::AllocationDiagnostics terminal = fixture.sampler.diagnostics();
    const bool stop_valid =
        stopped && terminal.accounting_state == spark::AllocationAccountingState::Complete &&
        terminal.processed_allocation_events == after_snapshot.processed_allocation_events &&
        terminal.processed_thread_observation_events == after_snapshot.processed_thread_observation_events &&
        terminal.processed_tick_events == after_snapshot.processed_tick_events &&
        terminal.discarded_allocation_events == after_snapshot.discarded_allocation_events &&
        terminal.discarded_thread_observation_events == after_snapshot.discarded_thread_observation_events &&
        terminal.discarded_tick_events == after_snapshot.discarded_tick_events &&
        terminal.drain_truncated == after_snapshot.drain_truncated &&
        terminal.caller_final_drain_allocation_events == after_snapshot.caller_final_drain_allocation_events &&
        terminal.caller_final_drain_thread_observation_events ==
            after_snapshot.caller_final_drain_thread_observation_events &&
        terminal.caller_final_drain_tick_events == after_snapshot.caller_final_drain_tick_events;
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && snapshot_valid && stop_valid ? true : report("fixture Linux process-all oracle failed");
#endif
}

bool verifyFixtureAggregatorAttribution()
{
    if (!supportedBackend()) {
        return true;
    }

    DiagnosticsFixture fixture;
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(makeFixtureConfig(0x2103), error)) {
        return report("fixture aggregator-attribution start failed");
    }
    spark::test::AllocationFixtureSeedCounts first_seed;
    const bool first_seed_ok =
        spark::test::AllocationDiagnosticsTestAccess::seedFixtureQueues(fixture.sampler, first_seed);
    const spark::AllocationDiagnostics before = fixture.sampler.diagnostics();
    const bool drained =
        first_seed_ok && spark::test::AllocationDiagnosticsTestAccess::drainFixtureAggregatorContext(fixture.sampler);
    const spark::AllocationDiagnostics after_aggregator = fixture.sampler.diagnostics();
    const bool aggregator_valid =
        first_seed.allocation_events == 3 && first_seed.thread_observation_events == 2 && first_seed.tick_events == 5 &&
        drained && exactDelta(after_aggregator.processed_allocation_events, before.processed_allocation_events, 3) &&
        exactDelta(after_aggregator.processed_thread_observation_events, before.processed_thread_observation_events,
                   2) &&
        exactDelta(after_aggregator.processed_tick_events, before.processed_tick_events, 5) &&
        after_aggregator.discarded_allocation_events == before.discarded_allocation_events &&
        after_aggregator.discarded_thread_observation_events == before.discarded_thread_observation_events &&
        after_aggregator.discarded_tick_events == before.discarded_tick_events &&
        after_aggregator.drain_truncated == before.drain_truncated &&
        after_aggregator.drain_truncated_allocation_events == before.drain_truncated_allocation_events &&
        after_aggregator.drain_truncated_thread_observation_events ==
            before.drain_truncated_thread_observation_events &&
        after_aggregator.drain_truncated_tick_events == before.drain_truncated_tick_events &&
        after_aggregator.caller_final_drain_allocation_events == before.caller_final_drain_allocation_events &&
        after_aggregator.caller_final_drain_thread_observation_events ==
            before.caller_final_drain_thread_observation_events &&
        after_aggregator.caller_final_drain_tick_events == before.caller_final_drain_tick_events &&
        after_aggregator.caller_final_drain_elapsed_ns == before.caller_final_drain_elapsed_ns &&
        after_aggregator.active_drain_elapsed_ns > before.active_drain_elapsed_ns;

    spark::test::AllocationFixtureSeedCounts second_seed;
    const bool second_seed_ok =
        spark::test::AllocationDiagnosticsTestAccess::seedFixtureQueues(fixture.sampler, second_seed);
    const spark::AllocationDiagnostics before_stop = fixture.sampler.diagnostics();
    const bool stopped = second_seed_ok && fixture.sampler.stop(error);
    const spark::AllocationDiagnostics terminal = fixture.sampler.diagnostics();
    const bool stop_valid =
        second_seed.allocation_events == 3 && second_seed.thread_observation_events == 2 &&
        second_seed.tick_events == 5 && stopped &&
        terminal.accounting_state == spark::AllocationAccountingState::Complete &&
        exactDelta(terminal.processed_allocation_events, before_stop.processed_allocation_events, 3) &&
        exactDelta(terminal.processed_thread_observation_events, before_stop.processed_thread_observation_events, 2) &&
        exactDelta(terminal.processed_tick_events, before_stop.processed_tick_events, 5) &&
        terminal.discarded_allocation_events == before_stop.discarded_allocation_events &&
        terminal.discarded_thread_observation_events == before_stop.discarded_thread_observation_events &&
        terminal.discarded_tick_events == before_stop.discarded_tick_events &&
        terminal.drain_truncated == before_stop.drain_truncated &&
        terminal.drain_truncated_allocation_events == before_stop.drain_truncated_allocation_events &&
        terminal.drain_truncated_thread_observation_events == before_stop.drain_truncated_thread_observation_events &&
        terminal.drain_truncated_tick_events == before_stop.drain_truncated_tick_events &&
        exactDelta(terminal.caller_final_drain_allocation_events, before_stop.caller_final_drain_allocation_events,
                   3) &&
        exactDelta(terminal.caller_final_drain_thread_observation_events,
                   before_stop.caller_final_drain_thread_observation_events, 2) &&
        exactDelta(terminal.caller_final_drain_tick_events, before_stop.caller_final_drain_tick_events, 5) &&
        terminal.caller_final_drain_elapsed_ns > before_stop.caller_final_drain_elapsed_ns &&
        terminal.active_drain_elapsed_ns == after_aggregator.active_drain_elapsed_ns &&
        terminal.processed_allocation_events - before.processed_allocation_events == 6 &&
        terminal.processed_thread_observation_events - before.processed_thread_observation_events == 4 &&
        terminal.processed_tick_events - before.processed_tick_events == 10;
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && aggregator_valid && stop_valid ? true : report("fixture aggregator-attribution oracle failed");
}

bool verifyFixtureRetainedBudget()
{
#if !defined(_WIN32) || !defined(_M_X64)
    return true;
#else
    DiagnosticsFixture fixture;
    spark::AllocationSamplerConfig config = makeConfig(0x2104);
    config.live_only = true;
    config.only_ticks_over_ms = 0;
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(config, error)) {
        return report("fixture retained-budget start failed");
    }
    std::array<void *, 8> pointers{};
    for (std::size_t i = 0; i < pointers.size(); ++i) {
        pointers[i] = fixture.backing.data() + i * 32;
    }
    const spark::AllocationDiagnostics before = fixture.sampler.diagnostics();
    const bool seeded = spark::test::AllocationDiagnosticsTestAccess::seedFixtureLiveAllocations(
        fixture.sampler, pointers.data(), pointers.size());
    const bool live_count = seeded && fixture.sampler.liveSamples() == pointers.size();
    spark::test::AllocationDiagnosticsTestAccess::forceRetainedWalkBudget(fixture.sampler, true);
    const bool stopped = fixture.sampler.stop(error);
    const spark::AllocationDiagnostics terminal = fixture.sampler.diagnostics();
    const std::uint64_t visited = spark::test::AllocationDiagnosticsTestAccess::retainedWalkVisits(fixture.sampler);
    spark::test::AllocationDiagnosticsTestAccess::forceRetainedWalkBudget(fixture.sampler, false);
    const bool valid =
        live_count && stopped && terminal.accounting_state == spark::AllocationAccountingState::Complete &&
        visited == 2 && exactDelta(terminal.retained_allocations_skipped, before.retained_allocations_skipped, 6) &&
        exactDelta(terminal.drain_truncated, before.drain_truncated, 6) &&
        terminal.processed_allocation_events == before.processed_allocation_events &&
        terminal.processed_thread_observation_events == before.processed_thread_observation_events &&
        terminal.processed_tick_events == before.processed_tick_events &&
        terminal.discarded_allocation_events == before.discarded_allocation_events &&
        terminal.discarded_thread_observation_events == before.discarded_thread_observation_events &&
        terminal.discarded_tick_events == before.discarded_tick_events &&
        terminal.drain_truncated_allocation_events == before.drain_truncated_allocation_events &&
        terminal.drain_truncated_thread_observation_events == before.drain_truncated_thread_observation_events &&
        terminal.drain_truncated_tick_events == before.drain_truncated_tick_events &&
        terminal.caller_final_drain_allocation_events == before.caller_final_drain_allocation_events &&
        terminal.caller_final_drain_thread_observation_events == before.caller_final_drain_thread_observation_events &&
        terminal.caller_final_drain_tick_events == before.caller_final_drain_tick_events &&
        terminal.caller_final_drain_elapsed_ns > before.caller_final_drain_elapsed_ns;
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && valid ? true : report("fixture retained-budget oracle failed");
#endif
}

bool verifyFixtureCapacityAccounting()
{
    if (!supportedBackend()) {
        return true;
    }

    DiagnosticsFixture fixture;
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(makeFixtureConfig(0x2105), error)) {
        return report("fixture capacity start failed");
    }

    const spark::AllocationDiagnostics before_pool = fixture.sampler.diagnostics();
    const bool pool_exercised =
        spark::test::AllocationDiagnosticsTestAccess::exerciseRecordPoolEmpty(fixture.sampler, fixture.backing.data());
    const spark::AllocationDiagnostics after_pool = fixture.sampler.diagnostics();
    const bool pool_valid =
        pool_exercised &&
        exactDelta(after_pool.record_pool_acquisition_failures, before_pool.record_pool_acquisition_failures, 1) &&
        after_pool.insertion_contention_failures == before_pool.insertion_contention_failures &&
        after_pool.exhausted_insertion_probe_failures == before_pool.exhausted_insertion_probe_failures &&
        after_pool.detach_contention_attempts == before_pool.detach_contention_attempts;

    spark::test::AllocationFixtureLiveRecord insertion_record;
    const bool prepared_insertion = spark::test::AllocationDiagnosticsTestAccess::prepareFixtureLiveRecord(
        fixture.sampler, fixture.backing.data() + 512, 1, insertion_record);
    const void *expected_opaque = insertion_record.opaque;
    fixture.launchLockHelper(insertion_record.pointer, true);
    const bool insertion_gate =
        prepared_insertion && expected_opaque != nullptr &&
        waitFor([&] { return fixture.lock_gate.ready.load(std::memory_order_acquire); }, std::chrono::seconds(2)) &&
        !fixture.lock_gate.timed_out.load(std::memory_order_acquire);
    const spark::AllocationDiagnostics before_insertion = fixture.sampler.diagnostics();
    const bool insertion_rejected =
        insertion_gate &&
        !spark::test::AllocationDiagnosticsTestAccess::insertFixtureLiveRecord(fixture.sampler, insertion_record, true);
    const spark::AllocationDiagnostics after_insertion = fixture.sampler.diagnostics();
    const bool insertion_valid =
        insertion_rejected &&
        exactDelta(after_insertion.insertion_contention_failures, before_insertion.insertion_contention_failures, 1) &&
        after_insertion.record_pool_acquisition_failures == before_insertion.record_pool_acquisition_failures &&
        after_insertion.exhausted_insertion_probe_failures == before_insertion.exhausted_insertion_probe_failures &&
        after_insertion.detach_contention_attempts == before_insertion.detach_contention_attempts;
    fixture.lock_gate.release.store(true, std::memory_order_release);
    fixture.joinHelpers();
    const bool insertion_succeeded =
        fixture.lock_ok &&
        spark::test::AllocationDiagnosticsTestAccess::insertFixtureLiveRecord(fixture.sampler, insertion_record, true);

    fixture.launchLockHelper(insertion_record.pointer, false);
    const bool detach_gate =
        insertion_succeeded &&
        waitFor([&] { return fixture.lock_gate.ready.load(std::memory_order_acquire); }, std::chrono::seconds(2)) &&
        !fixture.lock_gate.timed_out.load(std::memory_order_acquire);
    const spark::AllocationDiagnostics before_detach = fixture.sampler.diagnostics();
    const bool detach_rejected = detach_gate && !spark::test::AllocationDiagnosticsTestAccess::detachFixtureLiveRecord(
                                                    fixture.sampler, insertion_record);
    const spark::AllocationDiagnostics after_detach = fixture.sampler.diagnostics();
    const bool detach_valid =
        detach_rejected &&
        exactDelta(after_detach.detach_contention_attempts, before_detach.detach_contention_attempts, 1) &&
        after_detach.record_pool_acquisition_failures == before_detach.record_pool_acquisition_failures &&
        after_detach.insertion_contention_failures == before_detach.insertion_contention_failures &&
        after_detach.exhausted_insertion_probe_failures == before_detach.exhausted_insertion_probe_failures;
    fixture.lock_gate.release.store(true, std::memory_order_release);
    fixture.joinHelpers();
    const bool detach_succeeded =
        fixture.lock_ok &&
        spark::test::AllocationDiagnosticsTestAccess::detachFixtureLiveRecord(fixture.sampler, insertion_record);
    const bool identity_matches = detach_succeeded && insertion_record.opaque == expected_opaque;
    if (detach_succeeded) {
        spark::test::AllocationDiagnosticsTestAccess::retireFixtureLiveRecord(fixture.sampler, insertion_record);
    }
    else if (!insertion_succeeded) {
        spark::test::AllocationDiagnosticsTestAccess::releaseFixtureLiveRecord(fixture.sampler, insertion_record);
    }
    const bool live_empty = fixture.sampler.liveSamples() == 0;
    const bool stopped = fixture.sampler.stop(error);
    const spark::AllocationDiagnostics terminal = fixture.sampler.diagnostics();
    const bool terminal_valid =
        stopped && terminal.accounting_state == spark::AllocationAccountingState::Complete &&
        terminal.processed_allocation_events == before_pool.processed_allocation_events &&
        terminal.processed_thread_observation_events == before_pool.processed_thread_observation_events &&
        terminal.processed_tick_events == before_pool.processed_tick_events;
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && pool_valid && insertion_valid && insertion_succeeded && detach_valid && detach_succeeded &&
                   identity_matches && live_empty && terminal_valid
             ? true
             : report("fixture capacity oracle failed");
}

bool verifyFixtureProbeExhaustion()
{
    if (!supportedBackend()) {
        return true;
    }

    constexpr std::size_t live_index_shards = 64;
    constexpr std::size_t shard_capacity = 32768 / live_index_shards;
    constexpr std::size_t pointer_stride = live_index_shards * sizeof(std::uint64_t) * 2;
    constexpr std::size_t record_count = shard_capacity + 1;
    std::vector<unsigned char> backing(record_count * pointer_stride);
    DiagnosticsFixture fixture;
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(makeFixtureConfig(0x2106), error)) {
        return report("fixture probe start failed");
    }
    const spark::AllocationDiagnostics before = fixture.sampler.diagnostics();
    const bool exercised = spark::test::AllocationDiagnosticsTestAccess::exerciseInsertionProbeExhaustion(
        fixture.sampler, backing.data(), backing.size());
    const spark::AllocationDiagnostics after = fixture.sampler.diagnostics();
    const bool counters_valid =
        exercised &&
        exactDelta(after.exhausted_insertion_probe_failures, before.exhausted_insertion_probe_failures, 1) &&
        after.record_pool_acquisition_failures == before.record_pool_acquisition_failures &&
        after.insertion_contention_failures == before.insertion_contention_failures &&
        after.detach_contention_attempts == before.detach_contention_attempts && fixture.sampler.liveSamples() == 0;
    const bool stopped = fixture.sampler.stop(error);
    const spark::AllocationDiagnostics terminal = fixture.sampler.diagnostics();
    const bool terminal_valid = stopped && terminal.accounting_state == spark::AllocationAccountingState::Complete;
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && counters_valid && terminal_valid ? true : report("fixture probe oracle failed");
}

bool verifyFixtureWorker()
{
    if (!supportedBackend()) {
        return true;
    }

    DiagnosticsFixture fixture;
    std::string error;
    if (!fixture.configure(true, false, &fixture.worker_gate) ||
        !fixture.sampler.start(makeFixtureConfig(0x2006), error)) {
        return report("fixture worker start failed");
    }
    const bool worker_present = spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler);
    const bool entered =
        waitFor([&] { return fixture.worker_gate.entered.load(std::memory_order_acquire); }, std::chrono::seconds(2));
    if (!entered) {
        const bool cleanup_ok = fixture.cleanup();
        return cleanup_ok ? report("fixture worker gate was not entered") : report("fixture worker cleanup failed");
    }

    fixture.launchStopHelper();
    const bool incomplete_observed = waitFor(
        [&] { return fixture.sampler.diagnostics().accounting_state == spark::AllocationAccountingState::Incomplete; },
        std::chrono::seconds(2));
    fixture.worker_gate.release.store(true, std::memory_order_release);
    const bool stop_finished =
        waitFor([&] { return fixture.stop_done.load(std::memory_order_acquire); }, std::chrono::seconds(7));
    fixture.joinHelpers();
    const spark::AllocationDiagnostics terminal = fixture.sampler.diagnostics();
    const bool valid = worker_present && entered && incomplete_observed && stop_finished && fixture.stop_ok &&
                       terminal.accounting_state == spark::AllocationAccountingState::Complete &&
                       terminal.consumer_lifetime_elapsed_ns > 0 &&
                       !fixture.worker_gate.timed_out.load(std::memory_order_acquire) && !fixture.helperFailed();
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && valid ? true : report("fixture worker oracle failed");
}

bool runFixtureEarlyReturn(FixtureCleanupObservation &observation)
{
    DiagnosticsFixture fixture(&observation);
    std::string error;
    if (!fixture.configure(true, false, &fixture.worker_gate) ||
        !fixture.sampler.start(makeFixtureConfig(0x2007), error)) {
        return false;
    }
    if (!spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler)) {
        return false;
    }
    if (!waitFor([&] { return fixture.worker_gate.entered.load(std::memory_order_acquire); },
                 std::chrono::seconds(2))) {
        return false;
    }
    return true;
}

bool verifyFixtureEarlyReturn()
{
    if (!supportedBackend()) {
        return true;
    }

    FixtureCleanupObservation observation;
    const bool reached_gate = runFixtureEarlyReturn(observation);
    const bool valid = reached_gate && observation.cleanup_ok && observation.backing_was_valid &&
                       observation.worker_gate_released && !observation.worker_gate_timed_out;
    return valid ? true : report("fixture early-return cleanup oracle failed");
}

bool supportedBackend()
{
    return spark::AllocationSampler().diagnostics().supported;
}

bool finish(spark::AllocationSampler &sampler, std::string &error)
{
    return sampler.shutdown(error);
}

bool verifyCapacities()
{
    const spark::AllocationDiagnostics diagnostics = spark::AllocationSampler().diagnostics();
#if (defined(_WIN32) && defined(_M_X64)) || (defined(__linux__) && defined(__x86_64__))
    return diagnostics.supported && diagnostics.accounting_state == spark::AllocationAccountingState::NotStarted &&
           diagnostics.live_index_capacity == 32768 && diagnostics.live_record_capacity == 16384;
#else
    return !diagnostics.supported && diagnostics.accounting_state == spark::AllocationAccountingState::NotApplicable &&
           diagnostics.live_index_capacity == 0 && diagnostics.live_record_capacity == 0;
#endif
}

bool verifyPositiveAccounting()
{
    if (!supportedBackend()) {
        return true;
    }

    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config = makeConfig(0x1001);
    config.only_ticks_over_ms = 1;
    config.aggregator_delay_ms_for_testing = 100;
    std::string error;
    if (!sampler.start(config, error)) {
        return report("positive accounting start failed");
    }

    void *allocation = std::malloc(4096);
    if (allocation == nullptr) {
        finish(sampler, error);
        return report("positive accounting allocation failed");
    }
    static_cast<volatile unsigned char *>(allocation)[0] = 0x5a;
    for (int i = 0; i < 4; ++i) {
        sampler.onTick(50.0);
    }
    const bool enqueued = waitFor([&] { return sampler.enqueuedSamples() > 0; }, std::chrono::seconds(2));
    if (!sampler.stop(error)) {
        std::free(allocation);
        finish(sampler, error);
        return report("positive accounting stop failed");
    }

    const spark::AllocationDiagnostics diagnostics = sampler.diagnostics();
    const bool partition =
        sampler.enqueuedSamples() == diagnostics.processed_allocation_events + diagnostics.discarded_allocation_events;
    const bool truncated_partition =
        diagnostics.drain_truncated ==
        diagnostics.drain_truncated_allocation_events + diagnostics.drain_truncated_thread_observation_events +
            diagnostics.drain_truncated_tick_events + diagnostics.retained_allocations_skipped;
    const bool valid = enqueued && diagnostics.accounting_state == spark::AllocationAccountingState::Complete &&
                       sampler.enqueuedSamples() > 0 && partition && truncated_partition;
    std::free(allocation);
    if (!finish(sampler, error) || !valid) {
        return report("positive accounting oracle failed");
    }
    return true;
}

bool verifyStopSteadyStateTransition()
{
#if !defined(_WIN32) || !defined(_M_X64)
    return true;
#else
    const auto run_case = [](bool force_deadline, std::uint64_t seed, bool expect_processing) {
        DiagnosticsFixture fixture;
        std::string error;
        const bool configured = fixture.configure(true, false, &fixture.worker_gate);
        if (configured) {
            spark::test::AllocationDiagnosticsTestAccess::armEventProcessingGate(fixture.sampler, fixture.event_gate);
            if (force_deadline) {
                spark::test::AllocationDiagnosticsTestAccess::forceDrainDeadline(fixture.sampler, true);
            }
        }
        const bool started = configured && fixture.sampler.start(makeFixtureConfig(seed), error);
        const bool startup_entered =
            started && waitFor([&] { return fixture.worker_gate.entered.load(std::memory_order_acquire); },
                               std::chrono::seconds(2));
        spark::test::AllocationFixtureSeedCounts seeded;
        const std::uint64_t enqueued_before = fixture.sampler.enqueuedSamples();
        const bool seeded_ok =
            startup_entered && spark::test::AllocationDiagnosticsTestAccess::seedFixtureQueues(
                                   fixture.sampler, seeded,
                                   spark::test::AllocationFixtureSeedCounts{
                                       .allocation_events = 3, .thread_observation_events = 2, .tick_events = 0});
        const bool seed_valid = seeded_ok && seeded.allocation_events == 3 && seeded.thread_observation_events == 2 &&
                                seeded.tick_events == 0 && fixture.sampler.enqueuedSamples() == enqueued_before;
        fixture.worker_gate.release.store(true, std::memory_order_release);
        const bool event_gate_entered =
            seed_valid && waitFor([&] { return fixture.event_gate.entered.load(std::memory_order_acquire); },
                                  std::chrono::seconds(2));
        const bool aggregator_was_running =
            event_gate_entered &&
            waitFor(
                [&] { return spark::test::AllocationDiagnosticsTestAccess::fixtureAggregatorRunning(fixture.sampler); },
                std::chrono::seconds(2));
        if (aggregator_was_running) {
            fixture.launchStopHelper();
        }
        const bool aggregator_stopped =
            aggregator_was_running &&
            waitFor(
                [&] {
                    return !spark::test::AllocationDiagnosticsTestAccess::fixtureAggregatorRunning(fixture.sampler);
                },
                std::chrono::seconds(2));
        fixture.event_gate.release.store(true, std::memory_order_release);
        fixture.joinHelpers();
        const spark::AllocationDiagnostics terminal = fixture.sampler.diagnostics();
        const std::uint64_t expected_processed = expect_processing ? 3 : 0;
        const std::uint64_t expected_observations = expect_processing ? 2 : 0;
        const std::uint64_t expected_discarded = expect_processing ? 0 : 3;
        const std::uint64_t expected_discarded_observations = expect_processing ? 0 : 2;
        const bool valid =
            configured && started && startup_entered && seed_valid && event_gate_entered && aggregator_was_running &&
            aggregator_stopped && fixture.stop_done.load(std::memory_order_acquire) && fixture.stop_ok &&
            !spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler) &&
            fixture.stop_error.empty() && !fixture.sampler.stopWaitTimedOut() &&
            !fixture.worker_gate.timed_out.load(std::memory_order_acquire) &&
            !fixture.event_gate.timed_out.load(std::memory_order_acquire) &&
            terminal.accounting_state == spark::AllocationAccountingState::Complete &&
            terminal.processed_allocation_events == expected_processed &&
            terminal.processed_thread_observation_events == expected_observations &&
            terminal.processed_tick_events == 0 && terminal.discarded_allocation_events == expected_discarded &&
            terminal.discarded_thread_observation_events == expected_discarded_observations &&
            terminal.discarded_tick_events == 0 && terminal.drain_truncated_allocation_events == expected_discarded &&
            terminal.drain_truncated_thread_observation_events == expected_discarded_observations &&
            terminal.drain_truncated_tick_events == 0 &&
            terminal.drain_truncated == expected_discarded + expected_discarded_observations &&
            terminal.caller_final_drain_allocation_events == 0 &&
            terminal.caller_final_drain_thread_observation_events == 0 &&
            terminal.caller_final_drain_tick_events == 0 && fixture.sampler.dataIncomplete() == !expect_processing &&
            !fixture.helperFailed();
        const bool cleanup_ok = fixture.cleanup();
        const bool worker_absent = !spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler);
        if (force_deadline && cleanup_ok && worker_absent) {
            spark::test::AllocationDiagnosticsTestAccess::forceDrainDeadline(fixture.sampler, false);
        }
        return cleanup_ok && worker_absent && valid;
    };

    const bool healthy = run_case(false, 0x1003, true);
    const bool healthy_after_cleanup = run_case(false, 0x1004, true);
    const bool forced_deadline = run_case(true, 0x1005, false);
    return healthy && healthy_after_cleanup && forced_deadline ? true
                                                               : report("steady-state stop transition oracle failed");
#endif
}

bool verifyFailureAndStartState()
{
    if (!supportedBackend()) {
        return true;
    }

    std::string error;
    {
        DiagnosticsFixture fixture;
        const bool configured = fixture.configure(true, true);
        const bool started = configured && fixture.sampler.start(makeFixtureConfig(0x1008), error);
        spark::test::AllocationFixtureSeedCounts seeded;
        const bool seeded_ok =
            started && spark::test::AllocationDiagnosticsTestAccess::seedFixtureQueues(
                           fixture.sampler, seeded,
                           spark::test::AllocationFixtureSeedCounts{
                               .allocation_events = 1, .thread_observation_events = 0, .tick_events = 0});
        spark::test::AllocationDiagnosticsTestAccess::forceProcessEventFailure(fixture.sampler, true);
        bool injected_exception = false;
        bool arbitrary_exception = false;
        bool captured = false;
        if (seeded_ok) {
            spark::AllocationSnapshot snapshot;
            try {
                captured = fixture.sampler.snapshot(snapshot, error);
            }
            catch (const std::exception &exception) {
                injected_exception =
                    std::string_view(exception.what()) == "injected allocation event processing failure";
            }
            catch (...) {
                arbitrary_exception = true;
            }
        }
        spark::test::AllocationDiagnosticsTestAccess::forceProcessEventFailure(fixture.sampler, false);
        const spark::AllocationDiagnostics failure = fixture.sampler.diagnostics();
        const bool failure_valid =
            configured && started && seeded_ok && seeded.allocation_events == 1 &&
            seeded.thread_observation_events == 0 && seeded.tick_events == 0 && injected_exception &&
            !arbitrary_exception && !captured && failure.accounting_state == spark::AllocationAccountingState::Failed &&
            failure.processed_allocation_events == 1 && failure.processed_thread_observation_events == 0 &&
            failure.processed_tick_events == 0 && failure.consumer_lifetime_elapsed_ns == 0 &&
            failure.active_drain_elapsed_ns == 0 && failure.caller_final_drain_elapsed_ns == 0;
        const bool stopped = started && fixture.sampler.stop(error);
        const spark::AllocationDiagnostics after_stop = fixture.sampler.diagnostics();
        const bool stop_valid = stopped && after_stop.accounting_state == spark::AllocationAccountingState::Failed &&
                                !spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler);
        const bool restarted = stopped && fixture.sampler.start(makeFixtureConfig(0x1009), error);
        const spark::AllocationDiagnostics reset = fixture.sampler.diagnostics();
        const bool reset_valid = restarted && reset.accounting_state == spark::AllocationAccountingState::Active &&
                                 diagnosticsCountersZero(reset);
        const bool restart_stop = restarted && fixture.sampler.stop(error);
        const spark::AllocationDiagnostics restart_terminal = fixture.sampler.diagnostics();
        const bool restart_valid =
            restart_stop && restart_terminal.accounting_state == spark::AllocationAccountingState::Complete;
        const bool cleanup_ok = fixture.cleanup();
        if (!cleanup_ok || !failure_valid || !stop_valid || !reset_valid || !restart_valid) {
            return report("caller failure-state oracle failed");
        }
    }

    {
        DiagnosticsFixture fixture;
        const bool configured = fixture.configure(true, false, &fixture.worker_gate);
        spark::test::AllocationDiagnosticsTestAccess::forceProcessEventFailure(fixture.sampler, true);
        const bool started = configured && fixture.sampler.start(makeFixtureConfig(0x100a), error);
        const bool entered =
            started && waitFor([&] { return fixture.worker_gate.entered.load(std::memory_order_acquire); },
                               std::chrono::seconds(2));
        spark::test::AllocationFixtureSeedCounts seeded;
        const bool seeded_ok =
            entered && spark::test::AllocationDiagnosticsTestAccess::seedFixtureQueues(
                           fixture.sampler, seeded,
                           spark::test::AllocationFixtureSeedCounts{
                               .allocation_events = 1, .thread_observation_events = 0, .tick_events = 0});
        fixture.worker_gate.release.store(true, std::memory_order_release);
        const bool failure_seen = seeded_ok && waitFor(
                                                   [&] {
                                                       return fixture.sampler.diagnostics().accounting_state ==
                                                              spark::AllocationAccountingState::Failed;
                                                   },
                                                   std::chrono::seconds(2));
        if (started) {
            fixture.launchStopHelper();
        }
        fixture.joinHelpers();
        const bool worker_absent = !spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler);
        if (!worker_absent) {
            const bool cleanup_ok = fixture.cleanup();
            return cleanup_ok ? report("aggregator failure worker remained joinable")
                              : report("aggregator failure cleanup failed");
        }
        spark::test::AllocationDiagnosticsTestAccess::forceProcessEventFailure(fixture.sampler, false);
        const spark::AllocationDiagnostics failure = fixture.sampler.diagnostics();
        const bool failed_valid =
            configured && started && entered && seeded_ok && seeded.allocation_events == 1 &&
            seeded.thread_observation_events == 0 && seeded.tick_events == 0 && failure_seen && !fixture.stop_ok &&
            worker_absent && failure.accounting_state == spark::AllocationAccountingState::Failed &&
            failure.processed_allocation_events == 1 && failure.processed_thread_observation_events == 0 &&
            failure.processed_tick_events == 0 && failure.consumer_lifetime_elapsed_ns > 0 &&
            failure.active_drain_elapsed_ns > 0 && !fixture.worker_gate.timed_out.load(std::memory_order_acquire);
        const bool shutdown_ok = fixture.cleanup();
        if (!shutdown_ok || !failed_valid || !fixture.stop_done.load(std::memory_order_acquire)) {
            return report("aggregator failure-state oracle failed");
        }
    }

    {
        DiagnosticsFixture fixture;
        const bool configured = fixture.configure(true, false, &fixture.worker_gate);
        fixture.start_failure_gate.before_thread_creation.store(false, std::memory_order_release);
        fixture.start_failure_gate.fail_now.store(false, std::memory_order_release);
        if (configured) {
            spark::test::AllocationLifecycleTestAccess::armThreadCreationFailure(fixture.sampler,
                                                                                 fixture.start_failure_gate);
            fixture.launchStartHelper(makeFixtureConfig(0x100b));
        }
        const bool gate_seen =
            configured &&
            waitFor([&] { return fixture.start_failure_gate.before_thread_creation.load(std::memory_order_acquire); },
                    std::chrono::seconds(2));
        fixture.start_failure_gate.fail_now.store(true, std::memory_order_release);
        fixture.joinHelpers();
        if (configured) {
            spark::test::AllocationLifecycleTestAccess::disarmThreadCreationFailure(fixture.sampler);
        }
        const spark::AllocationDiagnostics failed_start = fixture.sampler.diagnostics();
        const bool start_failed_valid = configured && gate_seen && !fixture.start_ok &&
                                        failed_start.accounting_state == spark::AllocationAccountingState::Failed;
        const bool cleanup_ok = fixture.cleanup();
        if (!cleanup_ok || !start_failed_valid) {
            return report("start failure state oracle failed");
        }
    }
    return true;
}

bool verifyCountOnlyState()
{
    if (!supportedBackend()) {
        return true;
    }
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config = makeConfig(0x1010);
    config.count_only = true;
    std::string error;
    if (!sampler.start(config, error)) {
        return report("count-only diagnostics start failed");
    }
    const spark::AllocationDiagnostics active = sampler.diagnostics();
    const std::uint64_t before = sampler.observedBytes();
    void *allocation = std::malloc(4096);
    if (allocation == nullptr) {
        finish(sampler, error);
        return report("count-only diagnostics allocation failed");
    }
    static_cast<volatile unsigned char *>(allocation)[0] = 0x7;
    std::free(allocation);
    const bool stopped = sampler.stop(error);
    const spark::AllocationDiagnostics terminal = sampler.diagnostics();
    const auto timing_zero = [](const spark::AllocationDiagnostics &diagnostics) {
        const bool cpu_zero = !diagnostics.aggregator_cpu_valid && !diagnostics.aggregator_cpu_read_failure &&
                              diagnostics.aggregator_cpu_time_ns == 0;
#if defined(_WIN32) && defined(_M_X64)
        const bool cpu_status = diagnostics.aggregator_cpu_supported && cpu_zero;
#else
        const bool cpu_status = !diagnostics.aggregator_cpu_supported && cpu_zero;
#endif
        return diagnostics.consumer_lifetime_elapsed_ns == 0 && diagnostics.active_drain_elapsed_ns == 0 &&
               diagnostics.caller_final_drain_elapsed_ns == 0 && cpu_status;
    };
    const bool stop_valid = active.accounting_state == spark::AllocationAccountingState::NotApplicable && stopped &&
                            terminal.accounting_state == spark::AllocationAccountingState::NotApplicable &&
                            sampler.observedBytes() >= before + 4096 && sampler.sampleCount() == 0 &&
                            timing_zero(terminal);
    const bool shutdown_ok = finish(sampler, error);
    const spark::AllocationDiagnostics after_shutdown = sampler.diagnostics();
    const bool valid = stop_valid && shutdown_ok && timing_zero(after_shutdown) &&
                       after_shutdown.accounting_state == spark::AllocationAccountingState::NotApplicable;
    if (!valid) {
        return report("count-only state oracle failed");
    }
    return true;
}

bool verifyCpuAccounting()
{
#if !defined(_WIN32) || !defined(_M_X64)
    const spark::AllocationDiagnostics diagnostics = spark::AllocationSampler().diagnostics();
    const bool unsupported =
#if defined(__linux__) && defined(__x86_64__)
        diagnostics.supported && !diagnostics.aggregator_cpu_supported;
#else
        !diagnostics.supported && !diagnostics.aggregator_cpu_supported;
#endif
    const bool valid = unsupported && !diagnostics.aggregator_cpu_valid && !diagnostics.aggregator_cpu_read_failure &&
                       diagnostics.aggregator_cpu_time_ns == 0;
    return valid ? true : report("unsupported CPU accounting oracle failed");
#else
    std::string error;
    {
        DiagnosticsFixture fixture;
        if (!fixture.configure(true, false, &fixture.worker_gate)) {
            return report("CPU read-failure start failed");
        }
        spark::test::AllocationDiagnosticsTestAccess::forceAggregatorCpuReadFailure(fixture.sampler, true);
        const bool started = fixture.sampler.start(makeConfig(0x1013), error);
        const bool entered =
            started && waitFor([&] { return fixture.worker_gate.entered.load(std::memory_order_acquire); },
                               std::chrono::seconds(2));
        fixture.worker_gate.release.store(true, std::memory_order_release);
        const bool stopped = started && fixture.sampler.stop(error);
        fixture.joinHelpers();
        const bool worker_absent = !spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler);
        if (!worker_absent) {
            const bool cleanup_ok = fixture.cleanup();
            return cleanup_ok ? report("CPU read-failure worker remained joinable")
                              : report("CPU read-failure cleanup failed");
        }
        spark::test::AllocationDiagnosticsTestAccess::forceAggregatorCpuReadFailure(fixture.sampler, false);
        const spark::AllocationDiagnostics diagnostics = fixture.sampler.diagnostics();
        const bool valid = entered && stopped && worker_absent && diagnostics.aggregator_cpu_supported &&
                           !diagnostics.aggregator_cpu_valid && diagnostics.aggregator_cpu_read_failure &&
                           diagnostics.aggregator_cpu_time_ns == 0;
        const bool cleanup_ok = fixture.cleanup();
        if (!cleanup_ok || !valid) {
            return report("CPU read-failure oracle failed");
        }
    }

    {
        DiagnosticsFixture fixture;
        if (!fixture.configure(true, false, &fixture.worker_gate)) {
            return report("CPU equal-time start failed");
        }
        spark::test::AllocationDiagnosticsTestAccess::forceAggregatorCpuZero(fixture.sampler, true);
        const bool started = fixture.sampler.start(makeConfig(0x1014), error);
        const bool entered =
            started && waitFor([&] { return fixture.worker_gate.entered.load(std::memory_order_acquire); },
                               std::chrono::seconds(2));
        fixture.worker_gate.release.store(true, std::memory_order_release);
        const bool stopped = started && fixture.sampler.stop(error);
        fixture.joinHelpers();
        const bool worker_absent = !spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler);
        if (!worker_absent) {
            const bool cleanup_ok = fixture.cleanup();
            return cleanup_ok ? report("CPU equal-time worker remained joinable")
                              : report("CPU equal-time cleanup failed");
        }
        spark::test::AllocationDiagnosticsTestAccess::forceAggregatorCpuZero(fixture.sampler, false);
        const spark::AllocationDiagnostics diagnostics = fixture.sampler.diagnostics();
        const bool valid = entered && stopped && worker_absent && diagnostics.aggregator_cpu_supported &&
                           diagnostics.aggregator_cpu_valid && !diagnostics.aggregator_cpu_read_failure &&
                           diagnostics.aggregator_cpu_time_ns == 0;
        const bool cleanup_ok = fixture.cleanup();
        if (!cleanup_ok || !valid) {
            return report("CPU equal-time oracle failed");
        }
    }

    spark::AllocationDiagnostics cpu_without_sleep;
    spark::AllocationDiagnostics cpu_with_sleep;
    for (const std::uint32_t sleep_ms : {0U, 400U}) {
        DiagnosticsFixture fixture(nullptr, sleep_ms);
        const bool configured = fixture.configure(true, false, &fixture.worker_gate) && fixture.configureCpuWork();
        const bool started = configured && fixture.sampler.start(makeConfig(0x1015), error);
        const bool entered =
            started && waitFor([&] { return fixture.worker_gate.entered.load(std::memory_order_acquire); },
                               std::chrono::seconds(2));
        fixture.worker_gate.release.store(true, std::memory_order_release);
        const bool work_done = entered && waitFor([&] { return fixture.cpu_work.done.load(std::memory_order_acquire); },
                                                  std::chrono::seconds(7));
        const bool stopped = started && fixture.sampler.stop(error);
        fixture.joinHelpers();
        const spark::AllocationDiagnostics diagnostics = fixture.sampler.diagnostics();
        const bool work_valid =
            entered && work_done && stopped && !fixture.cpu_work.failed.load(std::memory_order_acquire) &&
            !fixture.cpu_work.cancelled.load(std::memory_order_acquire) &&
            fixture.cpu_work.checksum.load(std::memory_order_acquire) != 0 &&
            fixture.cpu_work.iterations.load(std::memory_order_acquire) > 0 && diagnostics.aggregator_cpu_supported &&
            diagnostics.aggregator_cpu_valid && !diagnostics.aggregator_cpu_read_failure &&
            diagnostics.aggregator_cpu_time_ns >= 40'000'000;
        if (sleep_ms == 0) {
            cpu_without_sleep = diagnostics;
        }
        else {
            cpu_with_sleep = diagnostics;
        }
        const bool cleanup_ok = fixture.cleanup();
        if (!cleanup_ok || !work_valid) {
            return report("CPU workload start/stop oracle failed");
        }
    }
    if (cpu_with_sleep.consumer_lifetime_elapsed_ns < cpu_without_sleep.consumer_lifetime_elapsed_ns ||
        cpu_with_sleep.consumer_lifetime_elapsed_ns - cpu_without_sleep.consumer_lifetime_elapsed_ns < 300'000'000) {
        return report("CPU workload lifetime difference oracle failed");
    }
    const std::uint64_t cpu_difference =
        cpu_without_sleep.aggregator_cpu_time_ns >= cpu_with_sleep.aggregator_cpu_time_ns
            ? cpu_without_sleep.aggregator_cpu_time_ns - cpu_with_sleep.aggregator_cpu_time_ns
            : cpu_with_sleep.aggregator_cpu_time_ns - cpu_without_sleep.aggregator_cpu_time_ns;
    if (cpu_difference >= 150'000'000) {
        return report("CPU workload CPU-time difference oracle failed");
    }
    return true;
#endif
}

bool verifyStateTransitions()
{
    if (!supportedBackend()) {
        return true;
    }

    std::string error;
    DiagnosticsFixture fixture;
#if defined(_WIN32) && defined(_M_X64)
    fixture.worker_gate.timeout_ms = 15000;
    const bool configured = fixture.configure(true, false, &fixture.worker_gate);
    const bool invalid_before = configured && !fixture.sampler.start(makeConfig(0), error);
    const spark::AllocationDiagnostics before_session = fixture.sampler.diagnostics();
    const bool invalid_before_valid = invalid_before &&
                                      before_session.accounting_state == spark::AllocationAccountingState::NotStarted &&
                                      diagnosticsCountersZero(before_session);
    const bool started = configured && fixture.sampler.start(makeConfig(0x1016), error);
    const bool entered = started && waitFor([&] { return fixture.worker_gate.entered.load(std::memory_order_acquire); },
                                            std::chrono::seconds(2));
    const spark::AllocationDiagnostics active = fixture.sampler.diagnostics();
    const bool active_valid = started && entered && active.accounting_state == spark::AllocationAccountingState::Active;
    const bool redundant_start = started && !fixture.sampler.start(makeConfig(0x1017), error);
    const spark::AllocationDiagnostics after_redundant = fixture.sampler.diagnostics();
    const bool redundant_valid = redundant_start && diagnosticsEqual(active, after_redundant);
    if (started) {
        fixture.launchStopHelper();
    }
    const bool first_stop_finished =
        started && waitFor([&] { return fixture.stop_done.load(std::memory_order_acquire); }, std::chrono::seconds(5));
    fixture.joinHelpers();
    const spark::AllocationDiagnostics incomplete = fixture.sampler.diagnostics();
    const bool incomplete_valid = first_stop_finished && !fixture.stop_ok && fixture.sampler.stopWaitTimedOut() &&
                                  incomplete.accounting_state == spark::AllocationAccountingState::Incomplete &&
                                  spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler) &&
                                  !fixture.worker_gate.timed_out.load(std::memory_order_acquire);
    fixture.worker_gate.release.store(true, std::memory_order_release);
    const bool retry_stop = started && fixture.sampler.stop(error);
    const spark::AllocationDiagnostics complete = fixture.sampler.diagnostics();
    const bool complete_valid = retry_stop && complete.accounting_state == spark::AllocationAccountingState::Complete &&
                                !spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler) &&
                                !fixture.worker_gate.timed_out.load(std::memory_order_acquire);
    const bool invalid_after = retry_stop && !fixture.sampler.start(makeConfig(0), error);
    const spark::AllocationDiagnostics after_invalid = fixture.sampler.diagnostics();
    const bool invalid_after_valid = invalid_after && diagnosticsEqual(complete, after_invalid);
    const bool retry_worker_absent =
        retry_stop && !spark::test::AllocationDiagnosticsTestAccess::fixtureWorkerPresent(fixture.sampler);
    if (retry_worker_absent) {
        fixture.worker_gate.entered.store(false, std::memory_order_relaxed);
        fixture.worker_gate.release.store(false, std::memory_order_relaxed);
        fixture.worker_gate.timed_out.store(false, std::memory_order_relaxed);
    }
    const bool restarted = retry_worker_absent && fixture.sampler.start(makeConfig(0x1018), error);
    const bool restarted_entry =
        restarted &&
        waitFor([&] { return fixture.worker_gate.entered.load(std::memory_order_acquire); }, std::chrono::seconds(2));
    const spark::AllocationDiagnostics reset_active = fixture.sampler.diagnostics();
    const bool reset_active_valid =
        restarted && restarted_entry && reset_active.accounting_state == spark::AllocationAccountingState::Active &&
        diagnosticsCountersZero(reset_active) && !fixture.worker_gate.timed_out.load(std::memory_order_acquire);
    fixture.worker_gate.release.store(true, std::memory_order_release);
    const bool reset_stopped = restarted && fixture.sampler.stop(error);
    const spark::AllocationDiagnostics reset_complete = fixture.sampler.diagnostics();
    const bool reset_valid =
        reset_stopped && reset_complete.accounting_state == spark::AllocationAccountingState::Complete;
    const bool shutdown_ok = fixture.sampler.shutdown(error);
    const spark::AllocationDiagnostics after_shutdown = fixture.sampler.diagnostics();
    const bool shutdown_valid =
        shutdown_ok && after_shutdown.accounting_state == spark::AllocationAccountingState::Complete;
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && invalid_before_valid && active_valid && redundant_valid && incomplete_valid &&
                   complete_valid && invalid_after_valid && reset_active_valid && reset_valid && shutdown_valid
             ? true
             : report("state transition oracle failed");
#else
    const bool configured = fixture.configure(true, true);
    const bool invalid_before = configured && !fixture.sampler.start(makeConfig(0), error);
    const spark::AllocationDiagnostics before_session = fixture.sampler.diagnostics();
    const bool invalid_before_valid = invalid_before &&
                                      before_session.accounting_state == spark::AllocationAccountingState::NotStarted &&
                                      diagnosticsCountersZero(before_session);
    const bool started = configured && fixture.sampler.start(makeConfig(0x1016), error);
    const spark::AllocationDiagnostics active = fixture.sampler.diagnostics();
    const bool active_valid = started && active.accounting_state == spark::AllocationAccountingState::Active;
    const bool redundant_start = started && !fixture.sampler.start(makeConfig(0x1017), error);
    const spark::AllocationDiagnostics after_redundant = fixture.sampler.diagnostics();
    const bool redundant_valid = redundant_start && diagnosticsEqual(active, after_redundant);
    if (started) {
        fixture.sampler.requestStop();
    }
    const spark::AllocationDiagnostics incomplete = fixture.sampler.diagnostics();
    const bool incomplete_valid =
        started && incomplete.accounting_state == spark::AllocationAccountingState::Incomplete;
    const bool retry_stop = started && fixture.sampler.stop(error);
    const spark::AllocationDiagnostics complete = fixture.sampler.diagnostics();
    const bool complete_valid = retry_stop && complete.accounting_state == spark::AllocationAccountingState::Complete;
    const bool invalid_after = retry_stop && !fixture.sampler.start(makeConfig(0), error);
    const spark::AllocationDiagnostics after_invalid = fixture.sampler.diagnostics();
    const bool invalid_after_valid = invalid_after && diagnosticsEqual(complete, after_invalid);
    const bool restarted = retry_stop && fixture.sampler.start(makeConfig(0x1018), error);
    const spark::AllocationDiagnostics reset_active = fixture.sampler.diagnostics();
    const bool reset_active_valid = restarted &&
                                    reset_active.accounting_state == spark::AllocationAccountingState::Active &&
                                    diagnosticsCountersZero(reset_active);
    const bool reset_stopped = restarted && fixture.sampler.stop(error);
    const spark::AllocationDiagnostics reset_complete = fixture.sampler.diagnostics();
    const bool reset_valid =
        reset_stopped && reset_complete.accounting_state == spark::AllocationAccountingState::Complete;
    const bool shutdown_ok = fixture.sampler.shutdown(error);
    const spark::AllocationDiagnostics after_shutdown = fixture.sampler.diagnostics();
    const bool shutdown_valid =
        shutdown_ok && after_shutdown.accounting_state == spark::AllocationAccountingState::Complete;
    const bool cleanup_ok = fixture.cleanup();
    return cleanup_ok && invalid_before_valid && active_valid && redundant_valid && incomplete_valid &&
                   complete_valid && invalid_after_valid && reset_active_valid && reset_valid && shutdown_valid
             ? true
             : report("state transition oracle failed");
#endif
}

bool verifyModuleCache()
{
#if defined(_WIN32) && defined(_M_X64)
    DiagnosticsFixture fixture;
    spark::AllocationSamplerConfig config = makeFixtureConfig(0x2107);
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(config, error)) {
        return report("module cache start failed");
    }
    const spark::AllocationDiagnostics before_anchor = fixture.sampler.diagnostics();
    const bool warmed = spark::test::AllocationDiagnosticsTestAccess::resolveFrameTwice(fixture.sampler);
    const spark::AllocationDiagnostics after_anchor = fixture.sampler.diagnostics();
    const bool warm_valid =
        warmed && exactDelta(after_anchor.module_cache_misses, before_anchor.module_cache_misses, 1) &&
        exactDelta(after_anchor.module_cache_hits, before_anchor.module_cache_hits, 1) &&
        after_anchor.module_cache_insertion_refusals == before_anchor.module_cache_insertion_refusals &&
        after_anchor.module_cache_size == before_anchor.module_cache_size + 1;

    const bool first_stop = fixture.sampler.stop(error);
    const bool reset_started = first_stop && fixture.sampler.start(makeFixtureConfig(0x2108), error);
    const spark::AllocationDiagnostics reset = fixture.sampler.diagnostics();
    const bool reset_valid = reset_started && reset.module_cache_size == 0 && reset.module_cache_hits == 0 &&
                             reset.module_cache_misses == 0 && reset.module_cache_insertion_refusals == 0;

    spark::test::AllocationDiagnosticsTestAccess::seedModuleCache(fixture.sampler,
                                                                  spark::AllocationSampler::moduleCacheCapacity());
    const spark::AllocationDiagnostics full = fixture.sampler.diagnostics();
    const bool full_valid = full.module_cache_size == 1024 && full.module_cache_insertion_refusals == 0;
    const bool missing_resolved = spark::test::AllocationDiagnosticsTestAccess::resolveMissingFrame(fixture.sampler);
    const spark::AllocationDiagnostics refused = fixture.sampler.diagnostics();
    const bool refusal_valid =
        missing_resolved && refused.module_cache_size == full.module_cache_size &&
        exactDelta(refused.module_cache_misses, full.module_cache_misses, 1) &&
        exactDelta(refused.module_cache_insertion_refusals, full.module_cache_insertion_refusals, 1) &&
        refused.module_cache_hits == full.module_cache_hits;

    const bool second_stop = fixture.sampler.stop(error);
    const bool second_reset_started = second_stop && fixture.sampler.start(makeFixtureConfig(0x2109), error);
    const spark::AllocationDiagnostics second_reset = fixture.sampler.diagnostics();
    const bool second_reset_valid = second_reset_started && second_reset.module_cache_size == 0 &&
                                    second_reset.module_cache_hits == 0 && second_reset.module_cache_misses == 0 &&
                                    second_reset.module_cache_insertion_refusals == 0;
    const bool anchor_once =
        second_reset_started && spark::test::AllocationDiagnosticsTestAccess::resolveFrameOnce(fixture.sampler);
    const spark::AllocationDiagnostics final_anchor = fixture.sampler.diagnostics();
    const bool final_valid = anchor_once && final_anchor.module_cache_misses == 1 &&
                             final_anchor.module_cache_hits == 0 && final_anchor.module_cache_size == 1 &&
                             final_anchor.module_cache_insertion_refusals == 0;
    const bool third_stop = fixture.sampler.stop(error);
    const bool cleanup_ok = fixture.cleanup();
    if (!cleanup_ok || !warm_valid || !reset_valid || !full_valid || !refusal_valid || !second_reset_valid ||
        !final_valid || !third_stop) {
        return report("module cache oracle failed");
    }
#else
    const auto diagnostics = spark::AllocationSampler().diagnostics();
    if (diagnostics.module_cache_supported || diagnostics.module_cache_size != 0 ||
        diagnostics.module_cache_hits != 0 || diagnostics.module_cache_misses != 0 ||
        diagnostics.module_cache_insertion_refusals != 0) {
        return report("unsupported module cache oracle failed");
    }
#endif
    return true;
}

bool verifyMainImageRange()
{
#if defined(_WIN32) && defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
    using Access = spark::test::AllocationDiagnosticsTestAccess;
    const auto limit = std::numeric_limits<std::uintptr_t>::max();
    if (!Access::mainImageRangeContains(100, 10, 100) || !Access::mainImageRangeContains(100, 10, 109) ||
        Access::mainImageRangeContains(100, 10, 99) || Access::mainImageRangeContains(100, 10, 110) ||
        Access::mainImageRangeContains(0, 10, 0) || Access::mainImageRangeContains(100, 0, 100) ||
        Access::mainImageRangeContains(limit - 9, 10, limit - 9) ||
        !Access::mainImageRangeContains(limit - 10, 10, limit - 10) ||
        !Access::mainImageRangeContains(limit - 10, 10, limit - 1) ||
        Access::mainImageRangeContains(limit - 10, 10, limit)) {
        return report("main image synthetic range oracle failed");
    }

    DiagnosticsFixture fixture;
    auto config = makeFixtureConfig(0x2110);
    std::string error;
    if (!fixture.configure(true, true) || !fixture.sampler.start(config, error)) {
        return report("main image fixture start failed");
    }
    const auto address = reinterpret_cast<std::uintptr_t>(&verifyMainImageRange);
    spark::test::AllocationMainImageState initial;
    if (!Access::mainImageState(fixture.sampler, initial) || initial.discoveries != 1 ||
        initial.fallback_queries != 0 || !Access::mainImageRangeContains(initial.base, initial.size, address)) {
        return report("main image discovery oracle failed");
    }
    spark::test::AllocationResolvedFrame fast;
    spark::test::AllocationResolvedFrame repeated;
    if (!Access::resolveFrame(fixture.sampler, address, false, fast) ||
        !Access::resolveFrame(fixture.sampler, address, false, repeated) || fast != repeated ||
        fast.raw_address != address || fast.rva != address - initial.base || fast.path == "unknown") {
        return report("main image repeated resolution oracle failed");
    }
    const auto fast_cache = fixture.sampler.diagnostics();
    spark::test::AllocationMainImageState state;
    if (!Access::mainImageState(fixture.sampler, state) || state.fallback_queries != 0 ||
        fast_cache.module_cache_misses != 1 || fast_cache.module_cache_hits != 1 || fast_cache.module_cache_size != 1 ||
        fast_cache.module_cache_insertion_refusals != 0) {
        return report("main image fast path query oracle failed");
    }
    spark::test::AllocationResolvedFrame fallback;
    if (!Access::resolveFrame(fixture.sampler, address, true, fallback) || fast != fallback ||
        !Access::mainImageState(fixture.sampler, state) || state.fallback_queries != 1 || state.base != initial.base ||
        state.size != initial.size || !Access::resolveFrame(fixture.sampler, address, false, repeated) ||
        fast != repeated || !Access::mainImageState(fixture.sampler, state) || state.fallback_queries != 1) {
        return report("main image forced fallback restoration oracle failed");
    }

    if (!fixture.sampler.stop(error) || !fixture.sampler.start(config, error) ||
        !Access::resolveFrame(fixture.sampler, address, true, fallback) ||
        !Access::resolveFrame(fixture.sampler, address, true, repeated) || fast != fallback || fast != repeated) {
        return report("main image cold fallback equivalence oracle failed");
    }
    const auto fallback_cache = fixture.sampler.diagnostics();
    if (!Access::mainImageState(fixture.sampler, state) || state.fallback_queries != 2 || state.discoveries != 1 ||
        state.base != initial.base || state.size != initial.size ||
        fallback_cache.module_cache_misses != fast_cache.module_cache_misses ||
        fallback_cache.module_cache_hits != fast_cache.module_cache_hits ||
        fallback_cache.module_cache_size != fast_cache.module_cache_size ||
        fallback_cache.module_cache_insertion_refusals != fast_cache.module_cache_insertion_refusals) {
        return report("main image cold cache equivalence oracle failed");
    }

    const HMODULE other_module = ::GetModuleHandleW(L"ntdll.dll");
    const auto other_address =
        reinterpret_cast<std::uintptr_t>(other_module != nullptr ? ::GetProcAddress(other_module, "NtClose") : nullptr);
    spark::test::AllocationResolvedFrame other;
    if (other_address == 0 || Access::mainImageRangeContains(state.base, state.size, other_address) ||
        !Access::resolveFrame(fixture.sampler, other_address, false, other) ||
        !Access::resolveFrame(fixture.sampler, other_address, false, repeated) || other != repeated ||
        other.path == "unknown" || other.module == fast.module || !Access::mainImageState(fixture.sampler, state) ||
        state.fallback_queries != 4) {
        return report("non-main module fallback query oracle failed");
    }
    const auto other_cache = fixture.sampler.diagnostics();
    if (other_cache.module_cache_misses != fallback_cache.module_cache_misses + 1 ||
        other_cache.module_cache_hits != fallback_cache.module_cache_hits + 1 ||
        other_cache.module_cache_size != fallback_cache.module_cache_size + 1 ||
        other_cache.module_cache_insertion_refusals != fallback_cache.module_cache_insertion_refusals) {
        return report("non-main module cache oracle failed");
    }

    if (!fixture.sampler.stop(error) || !Access::forceMainImageDiscoveryFailure(fixture.sampler, true) ||
        !fixture.sampler.start(config, error) || !Access::mainImageState(fixture.sampler, state) || state.base != 0 ||
        state.size != 0 || state.discoveries != 1 || state.fallback_queries != 0 ||
        !Access::resolveFrame(fixture.sampler, address, false, fallback) || fast != fallback) {
        return report("main image failed discovery reset oracle failed");
    }
    spark::test::AllocationResolvedFrame invalid;
    if (!Access::resolveFrame(fixture.sampler, limit, false, invalid) || invalid.path != "unknown" ||
        invalid.rva != limit || invalid.raw_address != limit ||
        !Access::resolveFrame(fixture.sampler, limit, true, repeated) || invalid != repeated ||
        !Access::mainImageState(fixture.sampler, state) || state.fallback_queries != 3) {
        return report("disabled main image fallback oracle failed");
    }
    if (!fixture.sampler.stop(error) || !Access::forceMainImageDiscoveryFailure(fixture.sampler, false) ||
        !fixture.sampler.start(config, error) || !Access::mainImageState(fixture.sampler, state) ||
        state.base != initial.base || state.size != initial.size || state.discoveries != 1 ||
        state.fallback_queries != 0 || !fixture.sampler.stop(error)) {
        return report("main image restart discovery oracle failed");
    }
    config.count_only = true;
    if (!fixture.sampler.start(config, error) || !Access::mainImageState(fixture.sampler, state) || state.base != 0 ||
        state.size != 0 || state.discoveries != 0 || state.fallback_queries != 0 || !fixture.sampler.stop(error) ||
        !fixture.cleanup()) {
        return report("count-only main image oracle failed");
    }
#endif
    return true;
}

bool verifyFileTimeConversion()
{
#if defined(_WIN32) && defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
    std::uint64_t value = 0;
    if (!spark::test::AllocationDiagnosticsTestAccess::fileTimeToNanoseconds(0, 10, value) || value != 1000 ||
        spark::test::AllocationDiagnosticsTestAccess::fileTimeToNanoseconds(
            std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(), value)) {
        return report("FILETIME conversion oracle failed");
    }
#endif
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    bool fixture_smoke_only = false;
    bool fixture_phase2_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--fixture-smoke") {
            fixture_smoke_only = true;
        }
        else if (std::string_view(argv[i]) == "--fixture-phase2") {
            fixture_phase2_only = true;
        }
        else {
            std::fprintf(stderr, "allocation diagnostics: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    const bool fixture_smoke = verifyFixtureOrdinary() && verifyFixtureSnapshot() && verifyFixtureReset() &&
                               verifyFixtureDirectRecord() && verifyFixtureWorker() && verifyFixtureEarlyReturn();
    if (fixture_smoke_only) {
        return fixture_smoke ? 0 : 1;
    }

    const bool fixture_phase2 = verifyFixtureWindowsBoundedAccounting() && verifyFixtureLinuxProcessAll() &&
                                verifyFixtureAggregatorAttribution() && verifyFixtureRetainedBudget() &&
                                verifyFixtureCapacityAccounting() && verifyFixtureProbeExhaustion() &&
                                verifyModuleCache() && verifyMainImageRange();
    if (fixture_phase2_only) {
        return fixture_smoke && fixture_phase2 ? 0 : 1;
    }

    const bool valid = fixture_smoke && fixture_phase2 && verifyCapacities() && verifyPositiveAccounting() &&
                       verifyStopSteadyStateTransition() && verifyFailureAndStartState() && verifyCountOnlyState() &&
                       verifyCpuAccounting() && verifyStateTransitions() && verifyFileTimeConversion();
    return valid ? 0 : 1;
}
