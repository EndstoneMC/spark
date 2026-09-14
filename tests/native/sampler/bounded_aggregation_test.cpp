#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "native/sampler/call_tree.h"
#include "native/sampler/sampler.h"
#include "native/sampler/types.h"

namespace spark {

struct SamplerTestAccess {
    static bool accept(Sampler &sampler, const Sample &sample) { return sampler.acceptSample(sample); }

    static void journalModuleDefinitions(Sampler &sampler, const Sample &sample)
    {
        sampler.journalModuleDefinitions(sample);
    }

    static void reset(Sampler &sampler) { sampler.resetSession(); }

    static void setRemainingStorage(Sampler &sampler, std::size_t nodes, std::size_t time_entries)
    {
        sampler.profile_nodes_remaining_ = nodes;
        sampler.profile_time_entries_remaining_ = time_entries;
    }

    static std::size_t profileNodesRemaining(const Sampler &sampler) { return sampler.profile_nodes_remaining_; }

    static std::size_t profileTimeEntriesRemaining(const Sampler &sampler)
    {
        return sampler.profile_time_entries_remaining_;
    }

    static void setOnlyTicksOver(Sampler &sampler, std::int64_t threshold)
    {
        sampler.config_.only_ticks_over_ms = threshold;
    }

    static void addPending(Sampler &sampler, std::uint64_t tick_id, Sample sample)
    {
        sampler.buckets_[tick_id].push_back(std::move(sample));
        ++sampler.pending_sample_count_;
    }

    static bool enqueueSample(Sampler &sampler, Sample sample) { return sampler.enqueueSample(std::move(sample)); }

    static std::size_t pendingSampleCount(const Sampler &sampler) { return sampler.pending_sample_count_; }

    static void setAggregatorThreadHook(Sampler &sampler, std::function<void()> hook)
    {
        sampler.aggregator_thread_hook_ = std::move(hook);
    }

    static void finishPending(Sampler &sampler, std::uint64_t terminal_tick) { sampler.finishPending(terminal_tick); }

    static void recordTickDecision(Sampler &sampler, std::uint64_t tick_id, bool keep)
    {
        sampler.recordTickDecision(tick_id, keep);
    }

    static void flushOrDrop(Sampler &sampler, std::uint64_t tick_id, bool keep) { sampler.flushOrDrop(tick_id, keep); }
};

}  // namespace spark

namespace {

class RecordingRecoverySink final : public spark::RecoverySink {
public:
    void journalModuleDef(std::uint32_t module_id, std::string_view path) override
    {
        events.push_back("module:" + std::to_string(module_id) + ":" + std::string(path));
    }

    void journalThreadDef(std::uint64_t, std::uint64_t, std::string_view) override { events.emplace_back("thread"); }
    void journalSample(const spark::Sample &) override { events.emplace_back("sample"); }
    void journalTickEvent(std::uint64_t, double) override { events.emplace_back("tick"); }

    std::vector<std::string> events;
};

spark::FrameKey frame(std::uint64_t rva)
{
    return {.module = 1, .rva = rva, .raw_address = rva};
}

spark::Sample pendingSample(std::uint64_t rva)
{
    spark::Sample result;
    result.thread_id = 1;
    result.thread_name = "thread";
    result.window = 1;
    result.frames = {frame(rva)};
    return result;
}

bool transactionalNodeAndTimeBudget()
{
    spark::CallTree tree;
    const std::vector<spark::FrameKey> frames = {frame(1), frame(2)};
    std::size_t nodes = 1;
    std::size_t times = 2;
    if (tree.logBounded(frames, 0, 7, nodes, times) || !tree.root().times.empty() || !tree.root().children.empty() ||
        nodes != 1 || times != 2) {
        return false;
    }

    nodes = 1;
    if (tree.logBounded(frames, 0, 7, nodes) || !tree.root().times.empty() || !tree.root().children.empty() ||
        nodes != 1) {
        return false;
    }

    nodes = 2;
    times = 3;
    if (!tree.logBounded(frames, 0, 7, nodes, times) || tree.root().times.at(0) != 7 || nodes != 0 || times != 0) {
        return false;
    }

    const spark::CallTree::StorageUsage usage = tree.storageUsage();
    return usage.child_nodes == 2 && usage.time_entries == 3;
}

bool prevalidatedMutationMatchesBoundedLogging()
{
    const std::vector<spark::FrameKey> frames = {frame(4), frame(5), frame(6)};
    spark::CallTree bounded;
    spark::CallTree prevalidated;
    std::size_t bounded_nodes = 32;
    std::size_t bounded_times = 32;
    std::size_t prevalidated_nodes = 32;
    std::size_t prevalidated_times = 32;
    for (std::int32_t window = 0; window < 4; ++window) {
        const std::uint64_t weight = static_cast<std::uint64_t>(window + 1);
        const auto required = prevalidated.requiredStorage(frames, window);
        if (!bounded.logBounded(frames, window, weight, bounded_nodes, bounded_times) ||
            !prevalidated.logBoundedPrevalidated(frames, window, weight, required, prevalidated_nodes,
                                                 prevalidated_times)) {
            return false;
        }
    }
    if (bounded.sampleCount() != prevalidated.sampleCount() ||
        bounded.storageUsage().child_nodes != prevalidated.storageUsage().child_nodes ||
        bounded.storageUsage().time_entries != prevalidated.storageUsage().time_entries ||
        bounded.root().times != prevalidated.root().times || bounded_nodes != prevalidated_nodes ||
        bounded_times != prevalidated_times) {
        return false;
    }

    spark::CallTree rejected;
    const auto required = rejected.requiredStorage(frames, 0);
    std::size_t nodes = required.child_nodes - 1;
    std::size_t times = required.time_entries;
    return !rejected.logBoundedPrevalidated(frames, 0, 1, required, nodes, times) && rejected.root().times.empty() &&
           rejected.root().children.empty() && nodes == required.child_nodes - 1 && times == required.time_entries;
}

bool pruningReclaimsExactStorage()
{
    spark::CallTree tree;
    const std::vector<spark::FrameKey> frames = {frame(3)};
    tree.log(frames, 0, 1);
    tree.log(frames, 1, 1);
    if (tree.storageUsage().child_nodes != 1 || tree.storageUsage().time_entries != 4) {
        return false;
    }
    const spark::CallTree::StorageUsage first = tree.pruneBeforeWithUsage(1);
    if (first.child_nodes != 0 || first.time_entries != 2 || tree.storageUsage().time_entries != 2) {
        return false;
    }
    const spark::CallTree::StorageUsage second = tree.pruneBeforeWithUsage(2);
    return second.child_nodes == 1 && second.time_entries == 2 && tree.storageUsage().child_nodes == 0 &&
           tree.storageUsage().time_entries == 0;
}

bool boundedModulesAndSamplerConstants()
{
    spark::ModuleTable modules(3);
    const auto first = modules.intern("first");
    const auto second = modules.intern("second");
    const auto overflow = modules.intern("overflow");
    return first == 1 && second == 2 && overflow == 0 && modules.size() == 3 &&
           spark::Sampler::moduleCapacity() == 512 && spark::Sampler::threadRootCapacity() == 257 &&
           spark::Sampler::pendingSampleCapacity() == 32768 && spark::Sampler::profileNodeCapacity() == 131072 &&
           spark::Sampler::profileTimeEntryCapacity() == 2 * 1024 * 1024;
}

bool combinedTreeBudgetIsTransactional()
{
    spark::Sampler sampler;
    spark::SamplerTestAccess::reset(sampler);
    spark::SamplerTestAccess::setRemainingStorage(sampler, 1, 4);
    spark::Sample sample{.frames = {frame(1)}, .thread_id = 1, .thread_name = "thread", .window = 1};

    return !spark::SamplerTestAccess::accept(sampler, sample) && sampler.tree().root().times.empty() &&
           sampler.threadTrees().empty() && sampler.sampleCount() == 0 && sampler.droppedProfileSamples() == 1 &&
           sampler.droppedSamples() == 1 && sampler.profileStorageExhausted() && sampler.dataIncomplete();
}

bool newThreadPreflightUsesEmptyTree()
{
    spark::Sampler sampler;
    spark::SamplerTestAccess::reset(sampler);
    spark::SamplerTestAccess::setRemainingStorage(sampler, 2, 4);
    const spark::Sample first{.frames = {frame(9)}, .thread_id = 1, .thread_name = "first", .window = 1};
    const spark::Sample second{.frames = {frame(9)}, .thread_id = 2, .thread_name = "second", .window = 1};

    if (!spark::SamplerTestAccess::accept(sampler, first) ||
        spark::SamplerTestAccess::profileNodesRemaining(sampler) != 0 ||
        spark::SamplerTestAccess::profileTimeEntriesRemaining(sampler) != 0 || sampler.threadTrees().size() != 1 ||
        sampler.tree().storageUsage().child_nodes != 1 || sampler.tree().storageUsage().time_entries != 2 ||
        sampler.threadTrees().begin()->second.tree.storageUsage().child_nodes != 1 ||
        sampler.threadTrees().begin()->second.tree.storageUsage().time_entries != 2) {
        return false;
    }

    if (spark::SamplerTestAccess::accept(sampler, second)) {
        return false;
    }
    return spark::SamplerTestAccess::profileNodesRemaining(sampler) == 0 &&
           spark::SamplerTestAccess::profileTimeEntriesRemaining(sampler) == 0 && sampler.threadTrees().size() == 1 &&
           sampler.tree().sampleCount() == 1 && sampler.tree().storageUsage().child_nodes == 1 &&
           sampler.tree().storageUsage().time_entries == 2 && sampler.droppedProfileSamples() == 1 &&
           sampler.droppedSamples() == 1 && sampler.profileStorageExhausted() && sampler.dataIncomplete();
}

bool recoveryModuleDefinitionsPrecedeSamples()
{
    spark::Sampler sampler;
    spark::SamplerTestAccess::reset(sampler);
    RecordingRecoverySink sink;
    sampler.setRecoverySink(&sink);

    spark::Sample sample{
        .frames = {{.module = 4, .rva = 12, .raw_address = 12}},
        .recovery_module_definitions = {{.module_id = 4, .path = "module-four.so"}},
        .thread_id = 7,
        .thread_name = "thread",
        .window = 1,
    };
    spark::SamplerTestAccess::journalModuleDefinitions(sampler, sample);
    if (!spark::SamplerTestAccess::accept(sampler, sample)) {
        return false;
    }
    const std::vector<std::string> expected = {"module:4:module-four.so", "thread", "sample"};
    return sink.events == expected;
}

bool excessThreadsUseOverflowRoot()
{
    spark::Sampler sampler;
    spark::SamplerTestAccess::reset(sampler);
    for (std::uint64_t thread_id = 1; thread_id <= 257; ++thread_id) {
        spark::Sample sample{
            .frames = {frame(thread_id)}, .thread_id = thread_id, .thread_name = "thread", .window = 1};
        if (!spark::SamplerTestAccess::accept(sampler, sample)) {
            return false;
        }
    }

    const auto overflow = sampler.threadTrees().find(0);
    return sampler.threadTrees().size() == spark::Sampler::threadRootCapacity() &&
           overflow != sampler.threadTrees().end() && overflow->second.thread_name == "<other threads>" &&
           overflow->second.tree.sampleCount() == 1 && sampler.overflowThreadSamples() == 1 &&
           sampler.droppedSamples() == 0 && !sampler.dataIncomplete();
}

bool terminalTickClassification()
{
    spark::Sampler sampler;
    spark::SamplerTestAccess::reset(sampler);
    spark::SamplerTestAccess::setOnlyTicksOver(sampler, 10);
    for (std::uint64_t rva = 1; rva <= 4; ++rva) {
        spark::SamplerTestAccess::addPending(sampler, 7, pendingSample(rva));
    }
    spark::SamplerTestAccess::finishPending(sampler, 7);
    if (sampler.terminalInFlightTickSamplesDiscarded() != 4 || sampler.droppedPendingSamples() != 0 ||
        sampler.droppedSamples() != 0 || sampler.dataIncomplete() || !sampler.tree().root().times.empty()) {
        return false;
    }
    if (sampler.sampleCount() != 0) {
        return false;
    }

    spark::Sampler mixed;
    spark::SamplerTestAccess::reset(mixed);
    spark::SamplerTestAccess::setOnlyTicksOver(mixed, 10);
    spark::SamplerTestAccess::addPending(mixed, 6, pendingSample(2));
    spark::SamplerTestAccess::addPending(mixed, 7, pendingSample(3));
    spark::SamplerTestAccess::addPending(mixed, 8, pendingSample(4));
    spark::SamplerTestAccess::finishPending(mixed, 7);
    if (mixed.terminalInFlightTickSamplesDiscarded() != 1 || mixed.droppedPendingSamples() != 2 ||
        mixed.droppedSamples() != 2 || !mixed.dataIncomplete()) {
        return false;
    }

    spark::Sampler completed;
    spark::SamplerTestAccess::reset(completed);
    spark::SamplerTestAccess::setOnlyTicksOver(completed, 10);
    spark::SamplerTestAccess::recordTickDecision(completed, 0, false);
    spark::SamplerTestAccess::recordTickDecision(completed, 1, true);
    spark::SamplerTestAccess::addPending(completed, 0, pendingSample(5));
    spark::SamplerTestAccess::addPending(completed, 1, pendingSample(6));
    spark::SamplerTestAccess::flushOrDrop(completed, 0, false);
    spark::SamplerTestAccess::flushOrDrop(completed, 1, true);
    return completed.terminalInFlightTickSamplesDiscarded() == 0 && completed.droppedPendingSamples() == 0 &&
           completed.droppedSamples() == 0 && !completed.dataIncomplete() && completed.sampleCount() == 1 &&
           completed.tree().sampleCount() == 1;
}

bool terminalTickLifecycle()
{
#if !defined(_WIN32) && !defined(__linux__)
    return true;
#else
    using namespace std::chrono_literals;

    spark::Sampler sampler;
    spark::SamplerConfig config;
    config.interval_us = 5'000'000;
    config.only_ticks_over_ms = 10;
    sampler.setTarget(0);

    std::mutex mutex;
    std::condition_variable cv;
    bool hook_entered = false;
    bool release_hook = false;
    spark::SamplerTestAccess::setAggregatorThreadHook(sampler, [&] {
        std::unique_lock lock(mutex);
        hook_entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_hook; });
    });

    if (!sampler.start(config)) {
        return false;
    }
    bool hook_timeout = false;
    {
        std::unique_lock lock(mutex);
        hook_timeout = !cv.wait_for(lock, 2s, [&] { return hook_entered; });
        if (hook_timeout) {
            release_hook = true;
        }
    }
    if (hook_timeout) {
        cv.notify_all();
        sampler.stop();
        return false;
    }

    if (!spark::SamplerTestAccess::enqueueSample(sampler, pendingSample(10))) {
        {
            std::scoped_lock lock(mutex);
            release_hook = true;
        }
        cv.notify_all();
        sampler.stop();
        return false;
    }

    std::atomic<bool> pause_started{false};
    std::thread pause_thread([&] {
        pause_started.store(true, std::memory_order_release);
        sampler.pauseForExport();
    });
    const auto pause_deadline = std::chrono::steady_clock::now() + 2s;
    while (!pause_started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < pause_deadline) {
        std::this_thread::yield();
    }
    if (!pause_started.load(std::memory_order_acquire)) {
        {
            std::scoped_lock lock(mutex);
            release_hook = true;
        }
        cv.notify_all();
        pause_thread.join();
        sampler.stop();
        return false;
    }
    {
        std::scoped_lock lock(mutex);
        release_hook = true;
    }
    cv.notify_all();
    pause_thread.join();

    if (spark::SamplerTestAccess::pendingSampleCount(sampler) != 1 || sampler.sampleCount() != 0 ||
        sampler.terminalInFlightTickSamplesDiscarded() != 0 || sampler.droppedSamples() != 0 ||
        sampler.dataIncomplete()) {
        sampler.stop();
        return false;
    }

    spark::SamplerTestAccess::setAggregatorThreadHook(sampler, {});
    if (!sampler.resumeAfterExport()) {
        sampler.stop();
        return false;
    }
    sampler.onTick(50.0);
    const auto admission_deadline = std::chrono::steady_clock::now() + 2s;
    while (sampler.sampleCount() != 1 && std::chrono::steady_clock::now() < admission_deadline) {
        std::this_thread::sleep_for(2ms);
    }
    if (sampler.sampleCount() != 1) {
        sampler.stop();
        return false;
    }
    if (sampler.numberOfTicks() != 1 || sampler.droppedSamples() != 0 || sampler.dataIncomplete()) {
        sampler.stop();
        return false;
    }

    spark::Sample current_sample = pendingSample(11);
    current_sample.tick_id = sampler.numberOfTicks();
    if (!spark::SamplerTestAccess::enqueueSample(sampler, std::move(current_sample))) {
        sampler.stop();
        return false;
    }
    sampler.pauseForExport();
    if (spark::SamplerTestAccess::pendingSampleCount(sampler) != 1) {
        sampler.stop();
        return false;
    }
    if (!sampler.stop() || sampler.terminalInFlightTickSamplesDiscarded() != 1 ||
        spark::SamplerTestAccess::pendingSampleCount(sampler) != 0 || sampler.sampleCount() != 1 ||
        sampler.droppedPendingSamples() != 0 || sampler.droppedSamples() != 0 || sampler.dataIncomplete()) {
        return false;
    }

    if (!sampler.stop() || sampler.terminalInFlightTickSamplesDiscarded() != 1 || sampler.sampleCount() != 1 ||
        sampler.droppedSamples() != 0 || sampler.dataIncomplete()) {
        return false;
    }

    if (!sampler.start(config) || !sampler.stop() || sampler.sampleCount() != 0 ||
        sampler.terminalInFlightTickSamplesDiscarded() != 0 || sampler.droppedSamples() != 0 ||
        sampler.numberOfTicks() != 0 || sampler.dataIncomplete()) {
        sampler.stop();
        return false;
    }
    return true;
#endif
}

bool terminalTickFailureCleanup()
{
#if !defined(_WIN32) && !defined(__linux__)
    return true;
#else
    using namespace std::chrono_literals;

    spark::Sampler sampler;
    spark::SamplerConfig config;
    config.interval_us = 5'000'000;
    config.only_ticks_over_ms = 10;
    sampler.setTarget(0);

    std::mutex mutex;
    std::condition_variable cv;
    bool hook_entered = false;
    bool release_hook = false;
    spark::SamplerTestAccess::setAggregatorThreadHook(sampler, [&] {
        std::unique_lock lock(mutex);
        hook_entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_hook; });
        throw std::runtime_error("injected sampler aggregator failure");
    });

    if (!sampler.start(config)) {
        return false;
    }
    bool hook_timeout = false;
    {
        std::unique_lock lock(mutex);
        hook_timeout = !cv.wait_for(lock, 2s, [&] { return hook_entered; });
        if (hook_timeout) {
            release_hook = true;
        }
    }
    if (hook_timeout) {
        cv.notify_all();
        sampler.stop();
        return false;
    }
    if (!spark::SamplerTestAccess::enqueueSample(sampler, pendingSample(12))) {
        {
            std::scoped_lock lock(mutex);
            release_hook = true;
        }
        cv.notify_all();
        sampler.stop();
        return false;
    }
    std::thread release_thread([&] {
        while (sampler.running()) {
            std::this_thread::yield();
        }
        {
            std::scoped_lock lock(mutex);
            release_hook = true;
        }
        cv.notify_all();
    });
    sampler.pauseForExport();
    release_thread.join();

    std::string failure;
    if (!sampler.failure(failure) || !sampler.stop() || sampler.terminalInFlightTickSamplesDiscarded() != 1 ||
        sampler.droppedSamples() != 0 || sampler.dataIncomplete() ||
        spark::SamplerTestAccess::pendingSampleCount(sampler) != 0) {
        sampler.stop();
        return false;
    }
    return sampler.stop() && sampler.terminalInFlightTickSamplesDiscarded() == 1;
#endif
}

}  // namespace

int main()
{
    if (!transactionalNodeAndTimeBudget() || !prevalidatedMutationMatchesBoundedLogging() ||
        !pruningReclaimsExactStorage() || !boundedModulesAndSamplerConstants() ||
        !combinedTreeBudgetIsTransactional() || !newThreadPreflightUsesEmptyTree() ||
        !recoveryModuleDefinitionsPrecedeSamples() || !excessThreadsUseOverflowRoot() ||
        !terminalTickClassification() || !terminalTickLifecycle() || !terminalTickFailureCleanup()) {
        std::fprintf(stderr, "bounded aggregation test failed\n");
        return 1;
    }
    return 0;
}
