#include "native/sampler/sampler.h"

#include <algorithm>
#include <chrono>
#include <string_view>
#include <utility>

#include <cpptrace/cpptrace.hpp>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on
#endif

#include "native/sampler/capture.h"
#include "native/sampler/thread_info.h"
#include "native/symbol/symbolicate.h"

namespace spark {

namespace {
// Leading frames to discard. Linux drops the SIGPROF handler and signal-return trampoline;
// Windows StackWalk64 reads the real context so nothing is dropped.
#ifdef _WIN32
constexpr std::size_t KLeadingDrop = 0;
#else
constexpr std::size_t kLeadingDrop = 2;
#endif

}  // namespace

Sampler::~Sampler()
{
    stop();
}

bool Sampler::start(const SamplerConfig &config)
{
    last_error_.clear();
    if (running_.load()) {
        last_error_ = "sampler is already running";
        return false;
    }
    config_ = config;
    if (!thread_selector_.configure(config_.all_threads, config_.regex_threads, config_.thread_patterns, last_error_)) {
        return false;
    }
    if (!Capture::arm()) {
        last_error_ = "the platform stack-capture backend could not be initialized";
        return false;
    }
    try {
        resetSession();
        start_time_ = std::chrono::steady_clock::now();
        running_.store(true);
        agg_running_.store(true);
        aggregator_thread_ = std::thread(&Sampler::aggregatorLoop, this);
        sampler_thread_ = std::thread(&Sampler::samplerLoop, this);
    }
    catch (...) {
        running_.store(false);
        wait_cv_.notify_all();
        if (sampler_thread_.joinable()) {
            sampler_thread_.join();
        }
        agg_running_.store(false);
        if (aggregator_thread_.joinable()) {
            aggregator_thread_.join();
        }
        Capture::disarm();
        last_error_ = "the sampler service threads could not be started";
        return false;
    }
    return true;
}

void Sampler::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    wait_cv_.notify_all();
    if (sampler_thread_.joinable()) {
        sampler_thread_.join();  // no more samples are produced after this
    }
    agg_running_.store(false);
    if (aggregator_thread_.joinable()) {
        aggregator_thread_.join();  // drains everything the sampler left behind
    }
    Capture::disarm();
}

void Sampler::pauseForExport()
{
    if (!running_.exchange(false)) {
        return;
    }
    wait_cv_.notify_all();
    if (sampler_thread_.joinable()) {
        sampler_thread_.join();
    }
    agg_running_.store(false);
    if (aggregator_thread_.joinable()) {
        aggregator_thread_.join();
    }
    // Intentionally do NOT disarm Capture or reset session data.
}

void Sampler::resumeAfterExport()
{
    if (running_.load()) {
        return;
    }
    // Capture backend is still armed from the original start(); session data
    // (tree, thread_trees, modules, start_time_) is preserved.
    running_.store(true);
    agg_running_.store(true);
    aggregator_thread_ = std::thread(&Sampler::aggregatorLoop, this);
    sampler_thread_ = std::thread(&Sampler::samplerLoop, this);
}

void Sampler::resetSession()
{
    Sample sample;
    while (samples_.try_dequeue(sample)) {
    }
    TickEvent tick;
    while (ticks_.try_dequeue(tick)) {
    }

    tree_ = CallTree{};
    thread_trees_.clear();
    buckets_.clear();
    tick_decisions_.clear();
    tick_decision_base_ = 0;
    window_sample_counts_.clear();
    next_history_prune_window_ = kHistoryPruneIntervalSeconds;
    modules_ = ModuleTable{};
    window_ticks_.clear();
    next_tick_history_prune_window_ = kHistoryPruneIntervalSeconds;
    current_tick_.store(0);
    sample_count_.store(0, std::memory_order_relaxed);
    sampler_tid_.store(0, std::memory_order_relaxed);
    aggregator_tid_.store(0, std::memory_order_relaxed);
    sampler_heartbeat_.sequence.store(0, std::memory_order_relaxed);
    sampler_heartbeat_.last_ns.store(0, std::memory_order_relaxed);
    aggregator_heartbeat_.sequence.store(0, std::memory_order_relaxed);
    aggregator_heartbeat_.last_ns.store(0, std::memory_order_relaxed);
    journaled_threads_.clear();
}

std::int32_t Sampler::currentWindow() const
{
    auto elapsed = std::chrono::steady_clock::now() - start_time_;
    return static_cast<std::int32_t>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
}

void Sampler::onTick(double mspt_ms)
{
    std::uint64_t finished = current_tick_.load();
    ticks_.enqueue(TickEvent{.tick_id = finished, .mspt_ms = mspt_ms});
    current_tick_.store(finished + 1);

    const std::int32_t window = currentWindow();
    WindowTickStats &w = window_ticks_[window];
    w.ticks += 1;
    w.mspt_sum += mspt_ms;
    w.mspt_max = std::max(mspt_ms, w.mspt_max);
    maybePruneTickHistory(window);
}

void Sampler::samplerLoop()
{
    struct ThreadTiming {
        std::chrono::steady_clock::time_point last_attempt;
        std::uint64_t previous_capture_us = 0;
    };

    CaptureBuffer buf;
    const auto interval = std::chrono::microseconds(config_.interval_us);
    sampler_tid_.store(currentNativeThreadId(), std::memory_order_release);
    while (running_.load() && aggregator_tid_.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    std::vector<ThreadInfo> targets;
    std::unordered_map<std::uint64_t, ThreadTiming> timings;
    std::size_t next_target = 0;
    auto next_refresh = std::chrono::steady_clock::time_point{};
    while (running_.load()) {
        {
            std::unique_lock lock(wait_mutex_);
            if (wait_cv_.wait_for(lock, interval, [this] { return !running_.load(); })) {
                break;
            }
        }

        if (config_.all_threads || !config_.thread_patterns.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_refresh) {
                targets = enumerateProcessThreads();
                const std::uint64_t sampler_tid = sampler_tid_.load(std::memory_order_acquire);
                const std::uint64_t aggregator_tid = aggregator_tid_.load(std::memory_order_acquire);
                auto removed = std::ranges::remove_if(targets, [&](const ThreadInfo &thread) {
                    return thread.id == sampler_tid || thread.id == aggregator_tid;
                });
                targets.erase(removed.begin(), removed.end());
                if (!config_.all_threads) {
                    removed = std::ranges::remove_if(
                        targets, [&](const ThreadInfo &thread) { return !thread_selector_.matches(thread.name); });
                    targets.erase(removed.begin(), removed.end());
                }
                std::erase_if(timings, [&](const auto &entry) {
                    return std::none_of(targets.begin(), targets.end(),
                                        [&](const ThreadInfo &thread) { return thread.id == entry.first; });
                });
                for (ThreadInfo &thread : targets) {
                    thread.name += " (#" + std::to_string(thread.id) + ")";
                }
                next_refresh = now + std::chrono::seconds(1);
            }
        }
        else {
            const std::uint64_t tid = target_tid_.load();
            targets = tid == 0 ? std::vector<ThreadInfo>{} : std::vector<ThreadInfo>{{.id = tid, .name = target_name_}};
        }

        const std::size_t target_count = targets.size();
        for (std::size_t checked = 0; checked < target_count; ++checked) {
            if (!running_.load()) {
                break;
            }
            const ThreadInfo &target = targets[next_target % target_count];
            ++next_target;

            const bool target_running = !config_.ignore_sleeping || Capture::isThreadRunning(target.id);
            const auto attempt_time = std::chrono::steady_clock::now();
            auto [timing_it, inserted] = timings.try_emplace(target.id);
            ThreadTiming &timing = timing_it->second;
            auto elapsed_us = static_cast<std::uint64_t>(config_.interval_us);
            if (!inserted) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::microseconds>(attempt_time - timing.last_attempt);
                const std::uint64_t wall_us = elapsed.count() > 0 ? static_cast<std::uint64_t>(elapsed.count()) : 1;
                elapsed_us = wall_us > timing.previous_capture_us ? wall_us - timing.previous_capture_us : 1;
            }
            timing.last_attempt = attempt_time;
            timing.previous_capture_us = 0;

            if (!target_running) {
                continue;
            }
            const bool captured = Capture::captureThread(target.id, buf);
            const auto capture_elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - attempt_time);
            if (capture_elapsed.count() > 0) {
                timing.previous_capture_us = static_cast<std::uint64_t>(capture_elapsed.count());
            }
            if (!captured) {
                break;  // one potentially expensive stack-walk attempt per interval
            }

            Sample sample;
            sample.thread_id = target.id;
            sample.thread_name = target.name;
            sample.tick_id = current_tick_.load();
            sample.window = currentWindow();
            sample.weight = elapsed_us;
            sample.frames.reserve(buf.count);
            for (std::size_t i = KLeadingDrop; i < buf.count; ++i) {
#ifdef _WIN32
                auto raw_address = static_cast<std::uint64_t>(buf.ips[i]);
                DWORD64 module_base = SymGetModuleBase64(GetCurrentProcess(), raw_address);

                std::string path = "unknown";
                if (module_base != 0) {
                    char module_path[MAX_PATH]{};
                    DWORD length =
                        // NOLINTNEXTLINE(performance-no-int-to-ptr)
                        GetModuleFileNameA(reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(module_base)),
                                           module_path, static_cast<DWORD>(sizeof(module_path)));
                    if (length > 0) {
                        path.assign(module_path, length);
                    }
                }

                FrameKey key;
                const std::size_t prev_module_count = modules_.size();
                key.module = modules_.intern(path);
                if (recovery_sink_ && modules_.size() > prev_module_count) {
                    recovery_sink_->journalModuleDef(key.module, path);
                }
                key.rva = module_base != 0 ? raw_address - module_base : raw_address;
                key.raw_address = raw_address;
#else
                cpptrace::safe_object_frame frame;
                cpptrace::get_safe_object_frame(buf.ips[i], &frame);
                std::string_view path =
                    frame.object_path[0] != '\0' ? std::string_view(frame.object_path) : std::string_view("unknown");
                FrameKey key;
                const std::size_t prev_module_count = modules_.size();
                key.module = modules_.intern(path);
                if (recovery_sink_ && modules_.size() > prev_module_count) {
                    recovery_sink_->journalModuleDef(key.module, path);
                }
                key.rva = static_cast<std::uint64_t>(frame.address_relative_to_object_start);
                key.raw_address = static_cast<std::uint64_t>(frame.raw_address);
#endif
                sample.frames.push_back(key);
            }
            if (sample.frames.empty()) {
                break;
            }
            // Drop inter-tick and worker wait states so they don't swamp a
            // wall-clock profile when sleeping threads are excluded.
            if (config_.ignore_sleeping && isSleepFrame(sample.frames.front().raw_address)) {
                break;
            }
            samples_.enqueue(std::move(sample));
            break;
        }
        sampler_heartbeat_.beat();
    }
    sampler_tid_.store(0, std::memory_order_release);
}

void Sampler::acceptSample(const Sample &sample)
{
    tree_.log(sample.frames, sample.window, sample.weight);
    auto [it, inserted] = thread_trees_.try_emplace(sample.thread_id);
    ThreadCallTree &thread = it->second;
    if (inserted) {
        thread.thread_id = sample.thread_id;
        thread.thread_name = sample.thread_name;
    }
    thread.tree.log(sample.frames, sample.window, sample.weight);
    ++window_sample_counts_[sample.window];
    sample_count_.fetch_add(1, std::memory_order_relaxed);
    maybePruneHistory(sample.window);

    if (recovery_sink_) {
        if (journaled_threads_.insert(sample.thread_id).second) {
            recovery_sink_->journalThreadDef(sample.thread_id, sample.thread_id, sample.thread_name);
        }
        recovery_sink_->journalSample(sample);
    }
}

void Sampler::maybePruneHistory(std::int32_t current_window)
{
    if (!config_.continuous || current_window < next_history_prune_window_) {
        return;
    }
    const std::int32_t minimum_window = current_window - kHistorySeconds;
    tree_.pruneBefore(minimum_window);
    for (auto &[thread_id, thread] : thread_trees_) {
        thread.tree.pruneBefore(minimum_window);
    }
    std::uint64_t removed_samples = 0;
    auto end = window_sample_counts_.lower_bound(minimum_window);
    for (auto it = window_sample_counts_.begin(); it != end; ++it) {
        removed_samples += it->second;
    }
    window_sample_counts_.erase(window_sample_counts_.begin(), end);
    sample_count_.fetch_sub(removed_samples, std::memory_order_relaxed);
    next_history_prune_window_ = current_window + kHistoryPruneIntervalSeconds;
}

void Sampler::maybePruneTickHistory(std::int32_t current_window)
{
    if (!config_.continuous || current_window < next_tick_history_prune_window_) {
        return;
    }
    const std::int32_t minimum_window = current_window - kHistorySeconds;
    window_ticks_.erase(window_ticks_.begin(), window_ticks_.lower_bound(minimum_window));
    next_tick_history_prune_window_ = current_window + kHistoryPruneIntervalSeconds;
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
    std::erase_if(buckets_, [this](const auto &entry) { return entry.first < tick_decision_base_; });
}

void Sampler::flushOrDrop(std::uint64_t tick_id, bool keep)
{
    auto it = buckets_.find(tick_id);
    if (it == buckets_.end()) {
        return;
    }
    if (keep) {
        for (const Sample &s : it->second) {
            acceptSample(s);
        }
    }
    buckets_.erase(it);
}

void Sampler::aggregatorLoop()
{
    aggregator_tid_.store(currentNativeThreadId(), std::memory_order_release);
    const bool ticked = config_.only_ticks_over_ms > 0;
    const auto threshold = static_cast<double>(config_.only_ticks_over_ms);

    auto drain = [&] {
        TickEvent ev;
        while (ticks_.try_dequeue(ev)) {
            bool keep = !ticked || ev.mspt_ms > threshold;
            if (ticked) {
                recordTickDecision(ev.tick_id, keep);
            }
            if (recovery_sink_) {
                recovery_sink_->journalTickEvent(ev.tick_id, ev.mspt_ms);
            }
            flushOrDrop(ev.tick_id, keep);
        }
        Sample s;
        while (samples_.try_dequeue(s)) {
            if (!ticked) {
                acceptSample(s);
            }
            else if (s.tick_id < tick_decision_base_) {
                continue;
            }
            else if (const auto offset = static_cast<std::size_t>(s.tick_id - tick_decision_base_);
                     offset < tick_decisions_.size() && tick_decisions_[offset] != 0) {
                if (tick_decisions_[offset] == 2) {
                    acceptSample(s);
                }
            }
            else {
                buckets_[s.tick_id].push_back(std::move(s));
            }
        }
    };

    while (agg_running_.load()) {
        drain();
        aggregator_heartbeat_.beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    drain();  // final: sampler has stopped, so this empties the queues
    aggregator_heartbeat_.beat();

    if (!ticked) {  // disabled => keep everything still buffered
        for (auto &[tick_id, samples] : buckets_) {
            for (const Sample &s : samples) {
                acceptSample(s);
            }
        }
    }
    buckets_.clear();
    aggregator_tid_.store(0, std::memory_order_release);
}

}  // namespace spark
