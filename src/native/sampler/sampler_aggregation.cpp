#include <algorithm>
#include <chrono>
#include <limits>
#include <string_view>

#include "native/sampler/sampler.h"
#include "native/sampler/thread_info.h"
#include "profiling_window.h"

namespace spark {

namespace {

constexpr std::string_view KOtherThreadsName = "<other threads>";

std::int32_t clampWindow(std::int64_t window) noexcept
{
    constexpr std::int64_t k_min = std::numeric_limits<std::int32_t>::min();
    constexpr std::int64_t k_max = std::numeric_limits<std::int32_t>::max();
    if (window < k_min) {
        return std::numeric_limits<std::int32_t>::min();
    }
    if (window > k_max) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(window);
}

}  // namespace

void Sampler::dropPendingSamples(std::size_t count) noexcept
{
    if (count == 0) {
        return;
    }
    dropped_pending_samples_.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
    dropped_samples_.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
}

void Sampler::finishPending(std::uint64_t terminal_tick)
{
    if (config_.only_ticks_over_ms <= 0) {
        for (const auto &[tick_id, samples] : buckets_) {
            (void)tick_id;
            for (const Sample &sample : samples) {
                acceptSample(sample);
            }
        }
        buckets_.clear();
        pending_sample_count_ = 0;
        return;
    }
    for (const auto &[tick_id, samples] : buckets_) {
        const std::size_t count = samples.size();
        if (tick_id == terminal_tick) {
            terminal_in_flight_tick_samples_discarded_.fetch_add(static_cast<std::uint64_t>(count),
                                                                 std::memory_order_relaxed);
        }
        else {
            dropPendingSamples(count);
        }
    }
    buckets_.clear();
    pending_sample_count_ = 0;
}

void Sampler::journalModuleDefinitions(const Sample &sample)
{
    if (recovery_sink_ == nullptr) {
        return;
    }
    for (const RecoveryModuleDefinition &definition : sample.recovery_module_definitions) {
        recovery_sink_->journalModuleDef(definition.module_id, definition.path);
    }
}

bool Sampler::acceptSample(const Sample &sample)
{
    const auto reject_profile_sample = [this] {
        dropped_profile_samples_.fetch_add(1, std::memory_order_relaxed);
        dropped_samples_.fetch_add(1, std::memory_order_relaxed);
        profile_storage_exhausted_.store(true, std::memory_order_relaxed);
        return false;
    };

    Sample normalized = sample;
    const auto identity = thread_identities_.find(sample.thread_id);
    if (identity != thread_identities_.end()) {
        normalized.thread_id = identity->second;
    }
    else if (sample.thread_id != 0 && thread_identities_.size() < kMaxThreadIdentities) {
        thread_identities_.emplace(sample.thread_id, sample.thread_id);
        normalized.thread_id = sample.thread_id;
    }
    else {
        normalized.thread_id = 0;
        normalized.thread_name = std::string(KOtherThreadsName);
        overflow_thread_samples_.fetch_add(1, std::memory_order_relaxed);
    }

    maybePruneHistory(normalized.window);
    const CallTree::StorageUsage global_required = tree_.requiredStorage(normalized.frames, normalized.window);
    auto thread_it = thread_trees_.find(normalized.thread_id);
    const bool new_thread = thread_it == thread_trees_.end();
    if (new_thread && thread_trees_.size() >= kThreadRootCapacity) {
        return reject_profile_sample();
    }
    const CallTree::StorageUsage thread_required =
        new_thread ? CallTree{}.requiredStorage(normalized.frames, normalized.window)
                   : thread_it->second.tree.requiredStorage(normalized.frames, normalized.window);
    if (global_required.child_nodes > profile_nodes_remaining_ ||
        thread_required.child_nodes > profile_nodes_remaining_ - global_required.child_nodes ||
        global_required.time_entries > profile_time_entries_remaining_ ||
        thread_required.time_entries > profile_time_entries_remaining_ - global_required.time_entries) {
        return reject_profile_sample();
    }

    if (new_thread) {
        auto inserted = thread_trees_.try_emplace(normalized.thread_id);
        thread_it = inserted.first;
        thread_it->second.thread_id = normalized.thread_id;
        thread_it->second.thread_name =
            normalized.thread_id == 0 ? std::string(KOtherThreadsName) : normalized.thread_name;
    }
    ThreadCallTree &thread = thread_it->second;
    const bool global_logged =
        tree_.logBoundedPrevalidated(normalized.frames, normalized.window, normalized.weight, global_required,
                                     profile_nodes_remaining_, profile_time_entries_remaining_);
    const bool thread_logged =
        thread.tree.logBoundedPrevalidated(normalized.frames, normalized.window, normalized.weight, thread_required,
                                           profile_nodes_remaining_, profile_time_entries_remaining_);
    if (!global_logged || !thread_logged) {
        // Preflight failure indicates inconsistent storage accounting.
        return reject_profile_sample();
    }

    ++window_sample_counts_[normalized.window];
    sample_count_.fetch_add(1, std::memory_order_relaxed);

    if (recovery_sink_) {
        if (journaled_threads_.insert(normalized.thread_id).second) {
            recovery_sink_->journalThreadDef(normalized.thread_id, sample.thread_id, normalized.thread_name);
        }
        // Python CodeIds are valid only for the lifetime of this interpreter/session
        // and intentionally have no ModuleDef in the stable recovery format. Preserve
        // crash recovery as native-only instead of writing unreplayable synthetic IDs.
        Sample recovery_sample = normalized;
        std::erase_if(recovery_sample.frames, [](const FrameKey &frame) { return isPythonFrame(frame); });
        recovery_sink_->journalSample(recovery_sample);
    }
    if (profile_nodes_remaining_ == 0 || profile_time_entries_remaining_ == 0) {
        profile_storage_exhausted_.store(true, std::memory_order_relaxed);
    }
    return true;
}

void Sampler::maybePruneHistory(std::int32_t current_window)
{
    if (current_window < next_history_prune_window_) {
        return;
    }
    const std::int32_t minimum_window = clampWindow(static_cast<std::int64_t>(current_window) -
                                                    static_cast<std::int64_t>(profiling_window::kHistorySize));
    const CallTree::StorageUsage released = tree_.pruneBeforeWithUsage(minimum_window);
    profile_nodes_remaining_ += released.child_nodes;
    profile_time_entries_remaining_ += released.time_entries;
    std::erase_if(thread_trees_, [&](auto &entry) {
        const CallTree::StorageUsage thread_released = entry.second.tree.pruneBeforeWithUsage(minimum_window);
        profile_nodes_remaining_ += thread_released.child_nodes;
        profile_time_entries_remaining_ += thread_released.time_entries;
        return entry.second.tree.root().times.empty() && entry.second.tree.root().children.empty();
    });

    std::uint64_t removed_samples = 0;
    auto end = window_sample_counts_.lower_bound(minimum_window);
    for (auto it = window_sample_counts_.begin(); it != end; ++it) {
        removed_samples += it->second;
    }
    window_sample_counts_.erase(window_sample_counts_.begin(), end);
    if (removed_samples != 0) {
        sample_count_.fetch_sub(removed_samples, std::memory_order_relaxed);
        history_samples_pruned_.fetch_add(removed_samples, std::memory_order_relaxed);
    }
    next_history_prune_window_ = static_cast<std::int64_t>(current_window) + kHistoryPruneIntervalWindows;
}

void Sampler::maybePruneTickHistory(std::int32_t current_window)
{
    if (current_window < next_tick_history_prune_window_) {
        return;
    }
    const std::int32_t minimum_window = clampWindow(static_cast<std::int64_t>(current_window) -
                                                    static_cast<std::int64_t>(profiling_window::kHistorySize));
    window_ticks_.erase(window_ticks_.begin(), window_ticks_.lower_bound(minimum_window));
    next_tick_history_prune_window_ = static_cast<std::int64_t>(current_window) + kHistoryPruneIntervalWindows;
}

void Sampler::recordTickDecision(std::uint64_t tick_id, bool keep)
{
    if (tick_id < tick_decision_base_) {
        return;
    }
    if (tick_id - tick_decision_base_ >= kTickDecisionCapacity) {
        const std::uint64_t new_base = tick_id - kTickDecisionCapacity + 1;
        const std::uint64_t remove_count = new_base - tick_decision_base_;
        if (remove_count >= tick_decisions_.size()) {
            tick_decisions_.clear();
        }
        else {
            tick_decisions_.erase(tick_decisions_.begin(),
                                  tick_decisions_.begin() + static_cast<std::ptrdiff_t>(remove_count));
        }
        tick_decision_base_ = new_base;
    }
    const auto offset = static_cast<std::size_t>(tick_id - tick_decision_base_);
    if (tick_decisions_.size() <= offset) {
        tick_decisions_.resize(offset + 1, 0);
    }
    tick_decisions_[offset] = keep ? 2 : 1;
    std::erase_if(buckets_, [this](auto &entry) {
        if (entry.first >= tick_decision_base_) {
            return false;
        }
        dropPendingSamples(entry.second.size());
        pending_sample_count_ -= entry.second.size();
        return true;
    });
}

void Sampler::flushOrDrop(std::uint64_t tick_id, bool keep)
{
    auto it = buckets_.find(tick_id);
    if (it == buckets_.end()) {
        return;
    }
    const std::size_t pending = it->second.size();
    pending_sample_count_ -= pending;
    if (keep) {
        for (const Sample &sample : it->second) {
            acceptSample(sample);
        }
    }
    buckets_.erase(it);
}

void Sampler::drainQueues()
{
    const bool ticked = config_.only_ticks_over_ms > 0;
    const auto threshold = static_cast<double>(config_.only_ticks_over_ms);
    TickEvent event;
    while (ticks_.try_dequeue(event)) {
        const bool keep = !ticked || event.mspt_ms > threshold;
        if (ticked) {
            recordTickDecision(event.tick_id, keep);
        }
        if (recovery_sink_) {
            recovery_sink_->journalTickEvent(event.tick_id, event.mspt_ms);
        }
        flushOrDrop(event.tick_id, keep);
    }
    Sample sample;
    while (samples_.try_dequeue(sample)) {
        journalModuleDefinitions(sample);
        if (!ticked) {
            acceptSample(sample);
        }
        else {
            bool pending_drop = sample.tick_id < tick_decision_base_;
            bool undecided = false;
            if (!pending_drop) {
                const auto offset = static_cast<std::size_t>(sample.tick_id - tick_decision_base_);
                const bool decided = offset < tick_decisions_.size() && tick_decisions_[offset] != 0;
                if (decided) {
                    if (tick_decisions_[offset] == 2) {
                        acceptSample(sample);
                    }
                }
                else if (pending_sample_count_ >= kMaxPendingSamples) {
                    pending_drop = true;
                }
                else {
                    undecided = true;
                }
            }
            if (pending_drop) {
                dropPendingSamples(1);
            }
            else if (undecided) {
                buckets_[sample.tick_id].push_back(std::move(sample));
                ++pending_sample_count_;
            }
        }
    }
}

void Sampler::aggregatorLoop()
{
    aggregator_tid_.store(currentNativeThreadId(), std::memory_order_release);

    while (agg_running_.load()) {
        drainQueues();
        aggregator_heartbeat_.beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    drainQueues();
    aggregator_heartbeat_.beat();

    if (finalize_pending_.load(std::memory_order_acquire)) {
        finishPending(terminal_tick_.load(std::memory_order_acquire));
        pending_finalized_.store(true, std::memory_order_release);
    }
    aggregator_tid_.store(0, std::memory_order_release);
}

}  // namespace spark
