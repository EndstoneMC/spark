#include "native/alloc/allocation_profile_aggregation.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "profiling_window.h"

namespace spark {

AllocationProfileAggregation::AllocationProfileAggregation() = default;

bool AllocationProfileAggregation::configure(std::string &error)
{
    return thread_filter_.configure(config_.all_threads, config_.regex_threads, config_.thread_patterns, error);
}

void AllocationProfileAggregation::reset(const AllocationSamplerConfig &config, RecoverySink *recovery_sink)
{
    config_ = config;
    recovery_sink_.store(recovery_sink, std::memory_order_release);
    thread_filter_.clear();
    tree_ = CallTree{};
    thread_trees_.clear();
    modules_ = ModuleTable{kModuleCapacity};
    window_ticks_.clear();
    pending_buckets_.clear();
    tick_decisions_.clear();
    high_tick_decisions_.clear();
    window_sample_counts_.clear();
    journaled_thread_roots_.fill(false);
    journaled_module_sentinel_ = false;
    pending_samples_ = 0;
    tick_event_seen_ = false;
    last_tick_event_id_ = 0;
    profile_nodes_remaining_ = kProfileNodeCapacity;
    profile_time_entries_remaining_ = kProfileTimeEntryCapacity;
    session_start_window_ = profiling_window::windowNow();
    last_history_window_ = session_start_window_;
    last_tick_window_ = session_start_window_;
    sample_count_.store(0, std::memory_order_relaxed);
    sampled_bytes_.store(0, std::memory_order_relaxed);
    dropped_samples_.store(0, std::memory_order_relaxed);
    dropped_profile_samples_.store(0, std::memory_order_relaxed);
    pending_drops_.store(0, std::memory_order_relaxed);
    pending_capacity_drops_.store(0, std::memory_order_relaxed);
    pending_stale_drops_.store(0, std::memory_order_relaxed);
    terminal_in_flight_tick_samples_discarded_.store(0, std::memory_order_relaxed);
    module_overflow_frames_.store(0, std::memory_order_relaxed);
    history_samples_pruned_.store(0, std::memory_order_relaxed);
    history_bytes_pruned_.store(0, std::memory_order_relaxed);
    profile_storage_exhausted_.store(false, std::memory_order_relaxed);
    journalSentinelModule();
}

AllocationThreadSelection AllocationProfileAggregation::resolveThread(std::uint64_t session_thread_id,
                                                                      std::uint64_t os_thread_id)
{
    return thread_filter_.resolve(session_thread_id, os_thread_id);
}

void AllocationProfileAggregation::observeThread(std::uint64_t session_thread_id, std::uint64_t os_thread_id)
{
    const AllocationThreadSelection selection = resolveThread(session_thread_id, os_thread_id);
    if (selection.selected && config_.observed_thread_identities_for_testing != nullptr) {
        config_.observed_thread_identities_for_testing->fetch_add(1, std::memory_order_release);
    }
}

FrameKey AllocationProfileAggregation::internFrame(std::string_view path, std::uint64_t rva, std::uint64_t raw_address)
{
    const std::size_t previous_size = modules_.size();
    const ModuleId module = modules_.intern(path);
    if (module == 0 && path != kOtherModulesSentinel && modules_.size() >= kModuleCapacity) {
        module_overflow_frames_.fetch_add(1, std::memory_order_relaxed);
        journalSentinelModule();
    }
    RecoverySink *sink = recovery_sink_.load(std::memory_order_acquire);
    if (sink != nullptr && modules_.size() > previous_size) {
        sink->journalModuleDef(module, path);
    }
    return FrameKey{.module = module, .rva = rva, .raw_address = raw_address};
}

bool AllocationProfileAggregation::processSample(Sample sample)
{
    if (config_.only_ticks_over_ms <= 0) {
        return acceptSample(std::move(sample));
    }
    if (sample.tick_id < tick_decisions_.size() && tick_decisions_[static_cast<std::size_t>(sample.tick_id)] != 0) {
        return tick_decisions_[static_cast<std::size_t>(sample.tick_id)] == 2 && acceptSample(std::move(sample));
    }
    if (sample.tick_id >= kMaxTickDecisions) {
        const auto decision = high_tick_decisions_.find(sample.tick_id);
        if (decision != high_tick_decisions_.end()) {
            if (decision->second) {
                return acceptSample(std::move(sample));
            }
            return false;
        }
        if (tick_event_seen_ && sample.tick_id <= last_tick_event_id_) {
            recordDrop(pending_stale_drops_);
            return false;
        }
        if (pending_samples_ >= kPendingSampleCapacity) {
            recordDrop(pending_capacity_drops_);
            return false;
        }
        pending_buckets_[sample.tick_id].push_back(std::move(sample));
        ++pending_samples_;
        return false;
    }
    if (pending_samples_ >= kPendingSampleCapacity) {
        recordDrop(pending_capacity_drops_);
        return false;
    }
    pending_buckets_[sample.tick_id].push_back(std::move(sample));
    ++pending_samples_;
    return false;
}

bool AllocationProfileAggregation::acceptSample(Sample sample)
{
    if (sample.frames.empty()) {
        recordDrop(dropped_profile_samples_);
        profile_storage_exhausted_.store(true, std::memory_order_relaxed);
        return false;
    }
    if (sample.thread_id > kNamedThreadRootCapacity) {
        sample.thread_id = 0;
        sample.thread_name = "<other threads>";
    }
    if (!admitToTrees(sample, tree_, thread_trees_, profile_nodes_remaining_, profile_time_entries_remaining_, nullptr,
                      nullptr)) {
        recordDrop(dropped_profile_samples_);
        profile_storage_exhausted_.store(true, std::memory_order_relaxed);
        return false;
    }
    if (profile_nodes_remaining_ == 0 || profile_time_entries_remaining_ == 0) {
        profile_storage_exhausted_.store(true, std::memory_order_relaxed);
    }

    sample_count_.fetch_add(1, std::memory_order_relaxed);
    sampled_bytes_.fetch_add(sample.weight, std::memory_order_relaxed);
    if (!config_.live_only) {
        WindowSampleStats &window = window_sample_counts_[sample.window];
        ++window.samples;
        window.bytes += sample.weight;
        if (sample.window > last_history_window_) {
            pruneHistory(sample.window, false);
        }
        else if (profiling_window::shouldPrune(sample.window, last_history_window_)) {
            pruneHistory(last_history_window_, true);
        }
    }
    journalThread(sample);
    RecoverySink *sample_sink = recovery_sink_.load(std::memory_order_acquire);
    if (sample_sink != nullptr) {
        sample_sink->journalSample(sample);
    }
    return true;
}

bool AllocationProfileAggregation::acceptLiveSample(Sample sample)
{
    sample.window = session_start_window_;
    return acceptSample(std::move(sample));
}

bool AllocationProfileAggregation::tickAccepts(std::uint64_t tick_id) const noexcept
{
    if (config_.only_ticks_over_ms <= 0) {
        return true;
    }
    if (tick_id < kMaxTickDecisions) {
        return tick_id < tick_decisions_.size() && tick_decisions_[static_cast<std::size_t>(tick_id)] == 2;
    }
    const auto decision = high_tick_decisions_.find(tick_id);
    return decision != high_tick_decisions_.end() && decision->second;
}

void AllocationProfileAggregation::processTick(std::uint64_t tick_id, double mspt_ms)
{
    const bool keep = config_.only_ticks_over_ms <= 0 || mspt_ms > static_cast<double>(config_.only_ticks_over_ms);
    if (config_.only_ticks_over_ms > 0 && tick_id < kMaxTickDecisions) {
        if (tick_decisions_.size() <= tick_id) {
            tick_decisions_.resize(static_cast<std::size_t>(tick_id + 1), 0);
        }
        tick_decisions_[static_cast<std::size_t>(tick_id)] = keep ? 2 : 1;
    }
    RecoverySink *tick_sink = recovery_sink_.load(std::memory_order_acquire);
    if (tick_sink != nullptr) {
        tick_sink->journalTickEvent(tick_id, mspt_ms);
    }
    if (config_.only_ticks_over_ms > 0 && tick_id >= kMaxTickDecisions) {
        tick_event_seen_ = true;
        last_tick_event_id_ = (std::max)(last_tick_event_id_, tick_id);
        high_tick_decisions_[tick_id] = keep;
        while (high_tick_decisions_.size() > kMaxTickDecisions) {
            high_tick_decisions_.erase(high_tick_decisions_.begin());
        }
        flushPending(tick_id, keep);
        dropStalePendingThrough(tick_id);
        return;
    }
    flushPending(tick_id, keep);
}

void AllocationProfileAggregation::recordTick(std::int32_t window, double mspt_ms)
{
    const std::int32_t effective_window = config_.live_only ? session_start_window_ : window;
    WindowTickStats &stats = window_ticks_[effective_window];
    ++stats.ticks;
    stats.mspt_sum += mspt_ms;
    stats.mspt_max = (std::max)(stats.mspt_max, mspt_ms);
    if (!config_.live_only && window > last_tick_window_) {
        last_tick_window_ = window;
        pruneTickHistory(window);
    }
}

bool AllocationProfileAggregation::tickDecisionObserved(std::uint64_t tick_id) const noexcept
{
    if (tick_id < kMaxTickDecisions) {
        return tick_id < tick_decisions_.size() && tick_decisions_[static_cast<std::size_t>(tick_id)] != 0;
    }
    return high_tick_decisions_.contains(tick_id);
}

void AllocationProfileAggregation::dropStalePendingThrough(std::uint64_t tick_id) noexcept
{
    for (auto it = pending_buckets_.begin(); it != pending_buckets_.end();) {
        if (it->first < kMaxTickDecisions || it->first > tick_id) {
            ++it;
            continue;
        }
        const std::size_t count = it->second.size();
        pending_stale_drops_.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
        pending_drops_.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
        dropped_samples_.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
        pending_samples_ -= count;
        it = pending_buckets_.erase(it);
    }
}

void AllocationProfileAggregation::classifyFinalTickSamples(std::uint64_t tick_id, std::uint64_t terminal_tick,
                                                            std::size_t count) noexcept
{
    if (count == 0) {
        return;
    }
    const bool decision_observed = tickDecisionObserved(tick_id);
    if (tick_id == terminal_tick && !decision_observed) {
        terminal_in_flight_tick_samples_discarded_.fetch_add(static_cast<std::uint64_t>(count),
                                                             std::memory_order_relaxed);
        return;
    }
    if (tick_id < terminal_tick && decision_observed) {
        return;
    }
    if (tick_id >= kMaxTickDecisions) {
        pending_stale_drops_.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
    }
    pending_drops_.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
    dropped_samples_.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
}

void AllocationProfileAggregation::finishPending(std::uint64_t terminal_tick)
{
    for (const auto &[tick_id, samples] : pending_buckets_) {
        classifyFinalTickSamples(tick_id, terminal_tick, samples.size());
    }
    pending_samples_ = 0;
    pending_buckets_.clear();
    if (!config_.live_only) {
        pruneHistory(profiling_window::windowNow(), true);
    }
}

void AllocationProfileAggregation::finishPending()
{
    if (pending_samples_ != 0) {
        const auto count = static_cast<std::uint64_t>(pending_samples_);
        pending_drops_.fetch_add(count, std::memory_order_relaxed);
        dropped_samples_.fetch_add(count, std::memory_order_relaxed);
        pending_samples_ = 0;
        pending_buckets_.clear();
    }
    if (!config_.live_only) {
        pruneHistory(profiling_window::windowNow(), true);
    }
}

bool AllocationProfileAggregation::copyCumulativeSnapshot(AllocationSnapshot &snapshot, std::uint64_t number_of_ticks,
                                                          std::string &error)
{
    error.clear();
    pruneHistory(profiling_window::windowNow(), true);
    snapshot = AllocationSnapshot{};
    snapshot.number_of_ticks = number_of_ticks;
    mergeCallTree(snapshot.tree, tree_);
    for (const auto &[id, source] : thread_trees_) {
        ThreadCallTree &target = snapshot.thread_trees[id];
        target.thread_id = source.thread_id;
        target.thread_name = source.thread_name;
        mergeCallTree(target.tree, source.tree);
    }
    snapshot.modules = modules_;
    snapshot.sample_count = sampleCount();
    snapshot.sampled_bytes = sampledBytes();
    return true;
}

bool AllocationProfileAggregation::buildLiveSnapshot(const std::vector<RetainedSample> &retained,
                                                     AllocationSnapshot &snapshot, std::uint64_t number_of_ticks,
                                                     std::string &error) const
{
    error.clear();
    CallTree tree;
    std::map<std::uint64_t, ThreadCallTree> thread_trees;
    std::size_t remaining_nodes = kProfileNodeCapacity;
    std::size_t remaining_time_entries = kProfileTimeEntryCapacity;
    std::uint64_t sample_count = 0;
    std::uint64_t sampled_bytes = 0;
    std::uint64_t total_age = 0;
    std::uint64_t maximum_age = 0;
    for (const RetainedSample &retained_sample : retained) {
        Sample sample = retained_sample.sample;
        if (!tickAccepts(sample.tick_id)) {
            continue;
        }
        sample.window = session_start_window_;
        if (!admitToTrees(sample, tree, thread_trees, remaining_nodes, remaining_time_entries, &sample_count,
                          &sampled_bytes)) {
            error = "allocation live snapshot profile storage exhausted";
            return false;
        }
        total_age += retained_sample.age_ms;
        maximum_age = (std::max)(maximum_age, retained_sample.age_ms);
    }

    snapshot = AllocationSnapshot{};
    snapshot.tree = std::move(tree);
    snapshot.thread_trees = std::move(thread_trees);
    snapshot.modules = modules_;
    snapshot.number_of_ticks = number_of_ticks;
    snapshot.sample_count = sample_count;
    snapshot.sampled_bytes = sampled_bytes;
    snapshot.retained_average_age_ms = sample_count == 0 ? 0 : total_age / sample_count;
    snapshot.retained_maximum_age_ms = maximum_age;
    return true;
}

bool AllocationProfileAggregation::admitToTrees(const Sample &sample, CallTree &tree,
                                                std::map<std::uint64_t, ThreadCallTree> &thread_trees,
                                                std::size_t &remaining_nodes, std::size_t &remaining_time_entries,
                                                std::uint64_t *sample_count, std::uint64_t *sampled_bytes)
{
    if (sample.frames.empty()) {
        return false;
    }
    const auto thread_id = sample.thread_id > kNamedThreadRootCapacity ? 0 : sample.thread_id;
    auto existing = thread_trees.find(thread_id);
    if (existing == thread_trees.end() && thread_trees.size() >= kThreadRootCapacity) {
        return false;
    }
    const CallTree::StorageUsage global_required = tree.requiredStorage(sample.frames, sample.window);
    const CallTree::StorageUsage thread_required =
        existing == thread_trees.end() ? CallTree{}.requiredStorage(sample.frames, sample.window)
                                       : existing->second.tree.requiredStorage(sample.frames, sample.window);
    if (global_required.child_nodes + thread_required.child_nodes > remaining_nodes ||
        global_required.time_entries + thread_required.time_entries > remaining_time_entries) {
        return false;
    }

    if (!tree.logBoundedPrevalidated(sample.frames, sample.window, sample.weight, global_required, remaining_nodes,
                                     remaining_time_entries)) {
        return false;
    }
    if (existing == thread_trees.end()) {
        ThreadCallTree thread;
        thread.thread_id = thread_id;
        thread.thread_name = thread_id == 0 ? "<other threads>" : sample.thread_name;
        existing = thread_trees.emplace(thread_id, std::move(thread)).first;
    }
    if (!existing->second.tree.logBoundedPrevalidated(sample.frames, sample.window, sample.weight, thread_required,
                                                      remaining_nodes, remaining_time_entries)) {
        return false;
    }
    if (sample_count != nullptr) {
        ++*sample_count;
    }
    if (sampled_bytes != nullptr) {
        *sampled_bytes += sample.weight;
    }
    return true;
}

void AllocationProfileAggregation::recordDrop(std::atomic<std::uint64_t> &counter) noexcept
{
    counter.fetch_add(1, std::memory_order_relaxed);
    if (&counter == &dropped_profile_samples_) {
        dropped_samples_.fetch_add(1, std::memory_order_relaxed);
    }
    else if (&counter == &pending_capacity_drops_ || &counter == &pending_stale_drops_) {
        pending_drops_.fetch_add(1, std::memory_order_relaxed);
        dropped_samples_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AllocationProfileAggregation::flushPending(std::uint64_t tick_id, bool keep)
{
    auto found = pending_buckets_.find(tick_id);
    if (found == pending_buckets_.end()) {
        return;
    }
    if (keep) {
        for (const Sample &sample : found->second) {
            (void)acceptSample(sample);
        }
    }
    pending_samples_ -= found->second.size();
    pending_buckets_.erase(found);
}

void AllocationProfileAggregation::pruneHistory(std::int32_t current_window, bool force)
{
    if (!force && current_window <= last_history_window_) {
        return;
    }
    const std::int64_t minimum_wide = static_cast<std::int64_t>(current_window) - profiling_window::kHistorySize;
    const auto minimum_window = static_cast<std::int32_t>(
        (std::max)(minimum_wide, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())));
    const auto end = window_sample_counts_.lower_bound(minimum_window);
    std::uint64_t removed_samples = 0;
    std::uint64_t removed_bytes = 0;
    for (auto it = window_sample_counts_.begin(); it != end; ++it) {
        removed_samples += it->second.samples;
        removed_bytes += it->second.bytes;
    }
    if (removed_samples != 0) {
        CallTree::StorageUsage released = tree_.pruneBeforeWithUsage(minimum_window);
        for (auto thread = thread_trees_.begin(); thread != thread_trees_.end();) {
            const CallTree::StorageUsage thread_released = thread->second.tree.pruneBeforeWithUsage(minimum_window);
            released.child_nodes += thread_released.child_nodes;
            released.time_entries += thread_released.time_entries;
            if (thread->second.tree.empty()) {
                thread = thread_trees_.erase(thread);
            }
            else {
                ++thread;
            }
        }
        profile_nodes_remaining_ = (std::min)(kProfileNodeCapacity, profile_nodes_remaining_ + released.child_nodes);
        profile_time_entries_remaining_ =
            (std::min)(kProfileTimeEntryCapacity, profile_time_entries_remaining_ + released.time_entries);
        history_samples_pruned_.fetch_add(removed_samples, std::memory_order_relaxed);
        history_bytes_pruned_.fetch_add(removed_bytes, std::memory_order_relaxed);
        sample_count_.fetch_sub(removed_samples, std::memory_order_relaxed);
        sampled_bytes_.fetch_sub(removed_bytes, std::memory_order_relaxed);
        window_sample_counts_.erase(window_sample_counts_.begin(), end);
    }
    last_history_window_ = (std::max)(last_history_window_, current_window);
}

void AllocationProfileAggregation::pruneTickHistory(std::int32_t current_window)
{
    const std::int64_t minimum_wide = static_cast<std::int64_t>(current_window) - profiling_window::kHistorySize;
    const auto minimum_window = static_cast<std::int32_t>(
        (std::max)(minimum_wide, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())));
    auto first = window_ticks_.begin();
    while (first != window_ticks_.end() && first->first < minimum_window) {
        first = window_ticks_.erase(first);
    }
}

void AllocationProfileAggregation::journalThread(const Sample &sample)
{
    const std::size_t root = sample.thread_id > kNamedThreadRootCapacity ? 0 : sample.thread_id;
    RecoverySink *sink = recovery_sink_.load(std::memory_order_acquire);
    if (journaled_thread_roots_[root] || sink == nullptr) {
        return;
    }
    journaled_thread_roots_[root] = true;
    sink->journalThreadDef(static_cast<std::uint64_t>(root), sample.os_thread_id,
                           root == 0 ? std::string_view("<other threads>") : sample.thread_name);
}

void AllocationProfileAggregation::journalSentinelModule()
{
    RecoverySink *sink = recovery_sink_.load(std::memory_order_acquire);
    if (journaled_module_sentinel_ || sink == nullptr) {
        return;
    }
    journaled_module_sentinel_ = true;
    sink->journalModuleDef(0, kOtherModulesSentinel);
}

}  // namespace spark
