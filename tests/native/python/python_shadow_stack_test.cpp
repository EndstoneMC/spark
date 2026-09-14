#include <atomic>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>

#include "native/python/python_attribution.h"

namespace {

bool expect(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "python shadow stack test: " << message << '\n';
    }
    return condition;
}

bool verifyLifecycleAndOverflow()
{
    spark::PythonShadowStack shadow;
    shadow.resetSession();
    constexpr std::uint64_t tid = 101;

    shadow.onEvent(tid, spark::PythonExecutionEvent::Start, 1);
    shadow.onEvent(tid, spark::PythonExecutionEvent::Start, 2);
    shadow.onEvent(tid, spark::PythonExecutionEvent::Start, 3);
    spark::PythonStackProvider::Snapshot snapshot;
    bool ok = true;
    ok &= expect(shadow.snapshot(tid, snapshot), "ordinary snapshot inconsistent");
    ok &= expect(snapshot.depth == 3 && snapshot.codes[0] == 1 && snapshot.codes[1] == 2 && snapshot.codes[2] == 3,
                 "ordinary root -> leaf stack is incorrect");
    shadow.onEvent(tid, spark::PythonExecutionEvent::Return, 3);
    shadow.onEvent(tid, spark::PythonExecutionEvent::Unwind, 2);
    shadow.onEvent(tid, spark::PythonExecutionEvent::Yield, 1);
    ok &= expect(shadow.snapshot(tid, snapshot) && snapshot.depth == 0,
                 "return/unwind/yield did not empty ordinary stack");

    for (std::uint64_t code = 1; code <= 300; ++code) {
        shadow.onEvent(tid, spark::PythonExecutionEvent::Start, code);
    }
    ok &= expect(shadow.snapshot(tid, snapshot), "overflow snapshot inconsistent");
    ok &= expect(snapshot.depth == spark::PythonStackProvider::kMaxDepth,
                 "overflow snapshot was not truncated to capacity");
    ok &= expect(shadow.maxDepth() == 300, "maximum logical depth is incorrect");
    ok &= expect(shadow.overflows() == 44, "overflow counter is incorrect");
    for (std::uint64_t code = 300; code >= 1; --code) {
        shadow.onEvent(tid, spark::PythonExecutionEvent::Return, code);
    }
    ok &=
        expect(shadow.snapshot(tid, snapshot) && snapshot.depth == 0, "overflow hidden-depth unwind left stale frames");

    shadow.onEvent(tid, spark::PythonExecutionEvent::Start, 10);
    shadow.onEvent(tid, spark::PythonExecutionEvent::Start, 20);
    shadow.onEvent(tid, spark::PythonExecutionEvent::Return, 10);
    ok &= expect(shadow.snapshot(tid, snapshot) && snapshot.depth == 0,
                 "mismatch recovery did not resynchronize to matched frame parent");
    ok &= expect(shadow.threadMismatches() > 0, "mismatch diagnostics were not incremented");
    return ok;
}

bool verifyConcurrentSnapshots()
{
    spark::PythonShadowStack shadow;
    shadow.resetSession();
    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> writer_tid{0};
    std::atomic<std::uint64_t> invalid_snapshots{0};
    std::atomic<std::uint64_t> successful_snapshots{0};

    std::thread writer([&] {
        const std::uint64_t tid = 202;
        writer_tid.store(tid, std::memory_order_release);
        for (int iteration = 0; iteration < 250'000; ++iteration) {
            shadow.onEvent(tid, spark::PythonExecutionEvent::Start, 11);
            shadow.onEvent(tid, spark::PythonExecutionEvent::Start, 22);
            shadow.onEvent(tid, spark::PythonExecutionEvent::Return, 22);
            shadow.onEvent(tid, spark::PythonExecutionEvent::Return, 11);
        }
        running.store(false, std::memory_order_release);
    });

    while (writer_tid.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    while (running.load(std::memory_order_acquire)) {
        spark::PythonStackProvider::Snapshot snapshot;
        if (!shadow.snapshot(writer_tid.load(std::memory_order_relaxed), snapshot)) {
            continue;
        }
        successful_snapshots.fetch_add(1, std::memory_order_relaxed);
        const bool valid = snapshot.depth == 0 || (snapshot.depth == 1 && snapshot.codes[0] == 11) ||
                           (snapshot.depth == 2 && snapshot.codes[0] == 11 && snapshot.codes[1] == 22);
        if (!valid) {
            invalid_snapshots.fetch_add(1, std::memory_order_relaxed);
        }
    }
    writer.join();

    spark::PythonStackProvider::Snapshot final_snapshot;
    bool ok = true;
    ok &= expect(shadow.snapshot(writer_tid.load(), final_snapshot), "final concurrent snapshot inconsistent");
    ok &= expect(final_snapshot.depth == 0, "writer completed with stale shadow frames");
    ok &= expect(successful_snapshots.load() > 0, "reader never obtained a consistent lock-free snapshot");
    ok &= expect(invalid_snapshots.load() == 0, "seqlock reader observed a torn Python stack");
    ok &= expect(shadow.snapshotAttempts() >= successful_snapshots.load(), "snapshot diagnostics are inconsistent");
    return ok;
}

bool verifyDistinctTidCapacityPerSession()
{
    spark::PythonShadowStack shadow;
    shadow.resetSession();
    bool ok = true;
    spark::PythonStackProvider::Snapshot snapshot;
    for (std::uint64_t tid = 1; tid <= spark::PythonShadowStack::kThreadCapacity; ++tid) {
        shadow.onEvent(tid, spark::PythonExecutionEvent::Start, tid);
        shadow.onEvent(tid, spark::PythonExecutionEvent::Return, tid);
        ok &= expect(shadow.snapshot(tid, snapshot) && snapshot.depth == 0,
                     "sequential native thread ID left an active Python frame");
    }
    ok &= expect(shadow.registeredThreads() == spark::PythonShadowStack::kThreadCapacity,
                 "distinct native thread IDs did not fill the session capacity");

    const auto mismatches = shadow.threadMismatches();
    shadow.onEvent(spark::PythonShadowStack::kThreadCapacity + 1, spark::PythonExecutionEvent::Start, 999);
    ok &= expect(shadow.threadMismatches() == mismatches + 1,
                 "a 257th distinct native thread ID was accepted before reset");

    shadow.resetSession();
    shadow.onEvent(spark::PythonShadowStack::kThreadCapacity + 1, spark::PythonExecutionEvent::Start, 1000);
    ok &= expect(shadow.registeredThreads() == 1 &&
                     shadow.snapshot(spark::PythonShadowStack::kThreadCapacity + 1, snapshot) && snapshot.depth == 1 &&
                     snapshot.codes[0] == 1000,
                 "session reset did not reclaim native thread ID capacity");
    return ok;
}

}  // namespace

int main()
{
    return verifyLifecycleAndOverflow() && verifyConcurrentSnapshots() && verifyDistinctTidCapacityPerSession() ? 0 : 1;
}
