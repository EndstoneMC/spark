#include "sampler/profiler.h"

#include <chrono>
#include <limits>
#include <string_view>

#include "proto/sampler_data.h"
#include "spark_constants.h"

namespace spark {
namespace {

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// spark-viewer parses every extra_platform_metadata value with JSON.parse().
// The protobuf field is map<string, string>, but each string must therefore be
// a complete JSON document rather than arbitrary display text.
std::string jsonString(std::string_view value)
{
    static constexpr char kHex[] = "0123456789abcdef";

    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20) {
                out += "\\u00";
                out.push_back(kHex[(ch >> 4) & 0x0f]);
                out.push_back(kHex[ch & 0x0f]);
            }
            else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string allocationHookSummary(
    const std::vector<AllocationHookCapability> &capabilities)
{
    std::string summary;
    for (const AllocationHookCapability &capability : capabilities) {
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += capability.name;
        summary += '=';
        summary += allocationHookStatusName(capability.status);
        if (!capability.detail.empty()) {
            summary += '(';
            summary += capability.detail;
            summary += ')';
        }
    }
    return summary;
}

}  // namespace

std::uint64_t Profiler::sampleCount() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.sampleCount() : sampler_.sampleCount();
}

std::uint64_t Profiler::sampledAllocationBytes() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.sampledBytes() : 0;
}

std::uint64_t Profiler::observedAllocationBytes() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.observedBytes() : 0;
}

std::uint64_t Profiler::droppedSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.droppedSamples() : 0;
}

std::uint64_t Profiler::freedAllocationSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.freedSamples() : 0;
}

std::uint64_t Profiler::liveAllocationSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.liveSamples() : 0;
}

std::uint64_t Profiler::liveAllocationBytes() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.liveBytes() : 0;
}

bool Profiler::backendFailure(std::string &error) const
{
    if (mode_ != ProfileMode::Allocation) {
        error.clear();
        return false;
    }
    return allocation_sampler_.failure(error);
}

const std::vector<AllocationHookCapability> &Profiler::allocationHookCapabilities() const
{
    return allocation_sampler_.hookCapabilities();
}

bool Profiler::start(const ProfilerOptions &options, std::uint64_t main_tid, std::string &error)
{
    if (running_.load()) {
        error = "profiler is already running";
        return false;
    }

    options_ = options;
    mode_ = options.alloc ? ProfileMode::Allocation : ProfileMode::Execution;

    const std::int64_t session_start_ms = nowMs();
    if (options.timeout_seconds > 0 &&
        options.timeout_seconds > (std::numeric_limits<std::int64_t>::max() - session_start_ms) / 1000) {
        error = "profiling timeout is too large";
        return false;
    }

    bool started = false;
    if (mode_ == ProfileMode::Allocation) {
        if (options.allocation_interval_bytes <= 0) {
            error = "allocation sampling interval must be greater than zero";
            return false;
        }
        interval_ = options.allocation_interval_bytes;
        AllocationSamplerConfig config;
        config.interval_bytes = options.allocation_interval_bytes;
        config.target_tid = main_tid;
        config.only_ticks_over_ms = options.only_ticks_over_ms > 0 ? options.only_ticks_over_ms : 0;
        config.live_only = options.alloc_live_only;
        config.fail_aggregator_for_testing = options.fail_allocation_aggregator_for_testing;
        started = allocation_sampler_.start(config, error);
    }
    else {
        const int interval_ms = options.interval_ms > 0 ? options.interval_ms : 4;
        if (interval_ms > kMaxSamplingIntervalMs) {
            error = "sampling interval must not exceed 1000 milliseconds";
            return false;
        }
        interval_ = interval_ms * 1000;

        SamplerConfig config;
        config.interval_us = interval_;
        config.ignore_sleeping = options.ignore_sleeping;
        config.only_ticks_over_ms = options.only_ticks_over_ms > 0 ? options.only_ticks_over_ms : 0;
        sampler_.setTarget(main_tid);
        started = sampler_.start(config);
        if (!started) {
            error = "the platform stack-capture backend could not be initialized";
        }
    }

    if (!started) {
        return false;
    }

    running_.store(true);
    start_time_ms_ = session_start_ms;
    cpu_baseline_ = captureCpuSnapshot();
    auto_end_time_ms_ = options.timeout_seconds > 0
                            ? start_time_ms_ + static_cast<std::int64_t>(options.timeout_seconds) * 1000
                            : -1;
    return true;
}

void Profiler::onTick(double mspt_ms)
{
    if (!running_.load()) {
        return;
    }
    if (mode_ == ProfileMode::Allocation) {
        allocation_sampler_.onTick(mspt_ms);
    }
    else {
        sampler_.onTick(mspt_ms);
    }
}

bool Profiler::stopSampling(std::string &error)
{
    error.clear();
    if (!running_.load()) {
        return true;
    }
    if (mode_ == ProfileMode::Allocation) {
        if (!allocation_sampler_.stop(error)) {
            if (!allocation_sampler_.running()) {
                running_.store(false);
            }
            return false;
        }
    }
    else {
        sampler_.stop();
    }
    running_.store(false);
    return true;
}

void Profiler::stopSampling()
{
    std::string ignored;
    stopSampling(ignored);
}

const CallTree &Profiler::activeTree() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.tree() : sampler_.tree();
}

const ModuleTable &Profiler::activeModules() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.modules() : sampler_.modules();
}

const std::map<std::int32_t, WindowTickStats> &Profiler::activeWindowTicks() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.windowTicks() : sampler_.windowTicks();
}

std::uint64_t Profiler::activeNumberOfTicks() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.numberOfTicks() : sampler_.numberOfTicks();
}

std::string Profiler::exportData(const ExportContext &ctx) const
{
    ProfileMetadata meta;
    meta.start_time_ms = start_time_ms_;
    meta.end_time_ms = nowMs();
    meta.interval = interval_;
    meta.mode = mode_;
    meta.number_of_ticks = static_cast<std::int32_t>(activeNumberOfTicks());
    meta.endstone_version = ctx.endstone_version;
    meta.minecraft_version = ctx.minecraft_version;
    meta.engine_version = mode_ == ProfileMode::Allocation
                              ? std::string("endstone-spark ") + kVersion + " native-ucrt/funchook"
                              : std::string("endstone-spark ") + kVersion;
    meta.comment = !ctx.comment.empty() ? ctx.comment : options_.comment;
    meta.creator_name = options_.creator_name;
    meta.creator_is_player = options_.creator_is_player;
    meta.ticked = options_.only_ticks_over_ms > 0;
    meta.tick_threshold_ms = options_.only_ticks_over_ms > 0 ? options_.only_ticks_over_ms : 0;

    if (mode_ == ProfileMode::Allocation) {
        // Upstream SamplerMetadata has no dedicated native allocation diagnostics.
        // The viewer JSON-parses every map value, so textual values must be encoded
        // as JSON string literals; numbers and booleans are already valid JSON.
        meta.extra_platform_metadata["Allocation backend"] = jsonString("Windows UCRT/funchook");
        meta.extra_platform_metadata["Allocation coverage"] = jsonString(
            "server thread; UCRT malloc/calloc/realloc plus recalloc/aligned/base and "
            "direct HeapAlloc/HeapReAlloc when available and hookable");
        meta.extra_platform_metadata["Allocation samples captured"] =
            std::to_string(allocation_sampler_.sampleCount());
        meta.extra_platform_metadata["Allocation samples dropped"] =
            std::to_string(allocation_sampler_.droppedSamples());
        meta.extra_platform_metadata["Allocation sampled frees"] =
            std::to_string(allocation_sampler_.freedSamples());
        meta.extra_platform_metadata["Allocation sampled freed bytes"] =
            std::to_string(allocation_sampler_.freedBytes());
        meta.extra_platform_metadata["Allocation sampled live allocations"] =
            std::to_string(allocation_sampler_.liveSamples());
        meta.extra_platform_metadata["Allocation sampled live bytes"] =
            std::to_string(allocation_sampler_.liveBytes());
        meta.extra_platform_metadata["Allocation average sampled lifetime ms"] =
            std::to_string(allocation_sampler_.averageLifetimeMs());
        meta.extra_platform_metadata["Allocation maximum sampled lifetime ms"] =
            std::to_string(allocation_sampler_.maximumLifetimeMs());
        meta.extra_platform_metadata["Allocation lifecycle records dropped"] =
            std::to_string(allocation_sampler_.lifecycleDropped());
        meta.extra_platform_metadata["Allocation sampled bytes"] =
            std::to_string(allocation_sampler_.sampledBytes());
        meta.extra_platform_metadata["Allocation observed request bytes"] =
            std::to_string(allocation_sampler_.observedBytes());
        meta.extra_platform_metadata["Allocation interval bytes"] = std::to_string(interval_);
        meta.extra_platform_metadata["Allocation live-only"] =
            options_.alloc_live_only ? "true" : "false";
        if (options_.alloc_live_only) {
            meta.extra_platform_metadata["Allocation analysis"] = jsonString(
                "retained sampled allocations at profile stop; candidates require repeated growth verification");
            meta.extra_platform_metadata["Allocation retained average age ms"] =
                std::to_string(allocation_sampler_.retainedAverageAgeMs());
            meta.extra_platform_metadata["Allocation retained maximum age ms"] =
                std::to_string(allocation_sampler_.retainedMaximumAgeMs());
        }

        const auto &capabilities = allocation_sampler_.hookCapabilities();
        std::size_t active = 0;
        std::size_t aliases = 0;
        for (const AllocationHookCapability &capability : capabilities) {
            active += capability.status == AllocationHookStatus::Active ? 1 : 0;
            aliases += capability.status == AllocationHookStatus::Alias ? 1 : 0;
        }
        meta.extra_platform_metadata["Allocation hook exports total"] =
            std::to_string(capabilities.size());
        meta.extra_platform_metadata["Allocation hook exports covered"] =
            std::to_string(active + aliases);
        meta.extra_platform_metadata["Allocation hook targets installed"] =
            std::to_string(active);
        meta.extra_platform_metadata["Allocation hook aliases"] =
            std::to_string(aliases);
        meta.extra_platform_metadata["Allocation hook capabilities"] =
            jsonString(allocationHookSummary(capabilities));
    }

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

    for (const auto &[window, wt] : activeWindowTicks()) {
        WindowStats ws;
        ws.ticks = wt.ticks;
        ws.tps = static_cast<double>(wt.ticks);
        ws.mspt_max = wt.mspt_max;
        ws.mspt_median = wt.ticks > 0 ? wt.mspt_sum / wt.ticks : 0.0;
        meta.window_stats[window] = ws;
    }

    const CallTree &tree = activeTree();
    std::vector<FrameKey> keys = collectFrameKeys(tree);
    auto resolved = resolveFrames(activeModules(), keys);
    return buildSamplerData(meta, tree, resolved);
}

std::string Profiler::stop(const ExportContext &ctx)
{
    if (!running_.load()) {
        return {};
    }
    std::string error;
    if (!stopSampling(error)) {
        return {};
    }
    return exportData(ctx);
}

bool Profiler::cancel(std::string &error)
{
    std::string stop_error;
    if (stopSampling(stop_error)) {
        error.clear();
        return true;
    }

    // A failed allocation aggregator invalidates the profile, but stopSampling
    // still joins the service thread, releases the event pool, and returns the
    // session to Idle. Cancelling intentionally discards that invalid data, so
    // completed cleanup is a successful cancel rather than another backend
    // failure presented to the user.
    std::string backend_error;
    if (!running_.load() && backendFailure(backend_error)) {
        error.clear();
        return true;
    }

    error = std::move(stop_error);
    return false;
}

void Profiler::cancel()
{
    std::string ignored;
    cancel(ignored);
}

bool Profiler::shutdown(std::string &error)
{
    error.clear();
    if (running_.load() && mode_ == ProfileMode::Allocation) {
        if (!allocation_sampler_.shutdown(error)) {
            return false;
        }
        running_.store(false);
        return true;
    }
    if (running_.load()) {
        sampler_.stop();
        running_.store(false);
    }
    return allocation_sampler_.shutdown(error);
}

}  // namespace spark
