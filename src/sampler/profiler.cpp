#include "sampler/profiler.h"

#include <chrono>

#include "proto/sampler_data.h"
#include "spark_constants.h"

namespace spark {

namespace {

long nowMs()
{
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count());
}

}  // namespace

bool Profiler::start(const ProfilerOptions &options, std::uint64_t main_tid, std::string &error)
{
    if (running_.load()) {
        error = "profiler is already running";
        return false;
    }

    options_ = options;
    interval_us_ = options.interval_ms > 0 ? options.interval_ms * 1000 : 4000;

    SamplerConfig config;
    config.interval_us = interval_us_;
    config.ignore_sleeping = options.ignore_sleeping;
    config.only_ticks_over_ms = options.only_ticks_over_ms > 0 ? options.only_ticks_over_ms : 0;

    sampler_.setTarget(main_tid);
    if (!sampler_.start(config)) {
        error = "async-signal-safe stack unwinding is unavailable (cpptrace must be built with libunwind)";
        return false;
    }

    running_.store(true);
    start_time_ms_ = nowMs();
    cpu_baseline_ = captureCpuSnapshot();  // CPU usage is measured over the profiling window
    auto_end_time_ms_ = options.timeout_seconds > 0 ? start_time_ms_ + options.timeout_seconds * 1000 : -1;
    return true;
}

void Profiler::onTick(double mspt_ms)
{
    if (running_.load()) {
        sampler_.onTick(mspt_ms);
    }
}

void Profiler::stopSampling()
{
    if (!running_.exchange(false)) {
        return;
    }
    sampler_.stop();
}

std::string Profiler::exportData(const ExportContext &ctx) const
{
    ProfileMetadata meta;
    meta.start_time_ms = start_time_ms_;
    meta.end_time_ms = nowMs();
    meta.interval_us = interval_us_;
    meta.number_of_ticks = static_cast<std::int32_t>(sampler_.numberOfTicks());
    meta.endstone_version = ctx.endstone_version;
    meta.minecraft_version = ctx.minecraft_version;
    meta.engine_version = std::string("endstone-spark ") + kVersion;
    meta.comment = !ctx.comment.empty() ? ctx.comment : options_.comment;
    meta.creator_name = options_.creator_name;
    meta.creator_is_player = options_.creator_is_player;
    meta.ticked = options_.only_ticks_over_ms > 0;
    meta.tick_threshold_ms = options_.only_ticks_over_ms > 0 ? options_.only_ticks_over_ms : 0;

    meta.platform_stats.present = true;
    meta.platform_stats.tps = ctx.tps;
    meta.platform_stats.mspt = ctx.mspt;
    meta.platform_stats.mspt_max = ctx.mspt_max;
    meta.platform_stats.player_count = ctx.player_count;
    meta.platform_stats.online_mode = ctx.online_mode;
    meta.platform_stats.uptime_ms = ctx.uptime_ms;
    meta.platform_stats.process_mem_bytes = processRssBytes();
    meta.platform_stats.process_virtual_bytes = processVirtualBytes();

    meta.system_stats = gatherSystemStats(cpu_baseline_, ".");
    meta.system_stats.uptime_ms = ctx.uptime_ms;
    meta.plugins = ctx.plugins;
    meta.world = ctx.world;

    for (const auto &[window, wt] : sampler_.windowTicks()) {
        WindowStats ws;
        ws.ticks = wt.ticks;
        ws.tps = static_cast<double>(wt.ticks);  // per-second window: tps == ticks recorded
        ws.mspt_max = wt.mspt_max;
        ws.mspt_median = wt.ticks > 0 ? wt.mspt_sum / wt.ticks : 0.0;
        meta.window_stats[window] = ws;
    }

    std::vector<FrameKey> keys = collectFrameKeys(sampler_.tree());
    auto resolved = resolveFrames(sampler_.modules(), keys);
    return buildSamplerData(meta, sampler_.tree(), resolved);
}

std::string Profiler::stop(const ExportContext &ctx)
{
    if (!running_.load()) {
        return {};
    }
    stopSampling();
    return exportData(ctx);
}

void Profiler::cancel()
{
    stopSampling();
}

}  // namespace spark
