#include "sampler/sampler.h"

#include <chrono>
#include <string_view>
#include <utility>

#include <cpptrace/cpptrace.hpp>

#include "sampler/capture.h"
#include "sampler/symbolicate.h"

namespace spark {

namespace {
// Leading frames to discard from each capture. On Linux the SIGPROF handler (frame 0)
// and the kernel signal-return trampoline (frame 1) sit above the real interrupted
// instruction (safe_generate_raw_trace already omits its own frame); this handler code
// path is identical in-plugin and in the self-test, so the count is stable. On Windows
// StackWalk64 reads the suspended thread's real context, so there is nothing to drop.
#if defined(_WIN32)
constexpr std::size_t kLeadingDrop = 0;
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
    if (running_.load()) {
        return false;
    }
    config_ = config;
    if (!Capture::arm()) {
        return false;
    }
    start_time_ = std::chrono::steady_clock::now();
    running_.store(true);
    agg_running_.store(true);
    aggregator_thread_ = std::thread(&Sampler::aggregatorLoop, this);
    sampler_thread_ = std::thread(&Sampler::samplerLoop, this);
    return true;
}

void Sampler::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    if (sampler_thread_.joinable()) {
        sampler_thread_.join();  // no more samples are produced after this
    }
    agg_running_.store(false);
    if (aggregator_thread_.joinable()) {
        aggregator_thread_.join();  // drains everything the sampler left behind
    }
    Capture::disarm();
}

std::int32_t Sampler::currentWindow() const
{
    auto elapsed = std::chrono::steady_clock::now() - start_time_;
    return static_cast<std::int32_t>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
}

void Sampler::onTick(double mspt_ms)
{
    std::uint64_t finished = current_tick_.load();
    ticks_.enqueue(TickEvent{finished, mspt_ms});
    current_tick_.store(finished + 1);

    WindowTickStats &w = window_ticks_[currentWindow()];
    w.ticks += 1;
    w.mspt_sum += mspt_ms;
    if (mspt_ms > w.mspt_max) {
        w.mspt_max = mspt_ms;
    }
}

void Sampler::samplerLoop()
{
    CaptureBuffer buf;
    const auto interval = std::chrono::microseconds(config_.interval_us);
    while (running_.load()) {
        std::this_thread::sleep_for(interval);

        std::uint64_t tid = target_tid_.load();
        if (tid == 0) {
            continue;
        }
        if (config_.ignore_sleeping && !Capture::isThreadRunning(tid)) {
            continue;
        }
        if (!Capture::captureThread(tid, buf)) {
            continue;
        }

        Sample sample;
        sample.tick_id = current_tick_.load();
        sample.window = currentWindow();
        sample.frames.reserve(buf.count);
        for (std::size_t i = kLeadingDrop; i < buf.count; ++i) {
            cpptrace::safe_object_frame frame;
            cpptrace::get_safe_object_frame(buf.ips[i], &frame);
            std::string_view path =
                frame.object_path[0] != '\0' ? std::string_view(frame.object_path) : std::string_view("unknown");
            FrameKey key;
            key.module = modules_.intern(path);
            key.rva = static_cast<std::uint64_t>(frame.address_relative_to_object_start);
            key.raw_address = static_cast<std::uint64_t>(frame.raw_address);
            sample.frames.push_back(key);
        }
        if (sample.frames.empty()) {
            continue;
        }
        // Drop the server thread's inter-tick idle wait (its innermost frame is a
        // sleep/wait function) so it doesn't swamp a wall-clock profile.
        if (config_.ignore_sleeping && isSleepFrame(sample.frames.front().raw_address)) {
            continue;
        }
        samples_.enqueue(std::move(sample));
    }
}

void Sampler::flushOrDrop(std::uint64_t tick_id, bool keep)
{
    auto it = buckets_.find(tick_id);
    if (it == buckets_.end()) {
        return;
    }
    if (keep) {
        for (const Sample &s : it->second) {
            tree_.log(s.frames, s.window);
        }
    }
    buckets_.erase(it);
}

void Sampler::aggregatorLoop()
{
    const bool ticked = config_.only_ticks_over_ms > 0;
    const double threshold = static_cast<double>(config_.only_ticks_over_ms);

    auto drain = [&] {
        TickEvent ev;
        while (ticks_.try_dequeue(ev)) {
            flushOrDrop(ev.tick_id, !ticked || ev.mspt_ms > threshold);
            // Straggler cleanup: any bucket whose tick clearly ended (well before the
            // one we just processed) will get no event, so resolve it now to bound memory.
            for (auto it = buckets_.begin(); it != buckets_.end();) {
                if (it->first + 8 < ev.tick_id) {
                    if (!ticked) {
                        for (const Sample &s : it->second) {
                            tree_.log(s.frames, s.window);
                        }
                    }
                    it = buckets_.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        Sample s;
        while (samples_.try_dequeue(s)) {
            buckets_[s.tick_id].push_back(std::move(s));
        }
    };

    while (agg_running_.load()) {
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    drain();  // final: sampler has stopped, so this empties the queues

    if (!ticked) {  // disabled => keep everything still buffered
        for (auto &[tick_id, samples] : buckets_) {
            for (const Sample &s : samples) {
                tree_.log(s.frames, s.window);
            }
        }
    }
    buckets_.clear();
}

}  // namespace spark
