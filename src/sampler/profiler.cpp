#include "sampler/profiler.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string_view>

#include "proto/sampler_data.h"
#include "spark_constants.h"
#if defined(_WIN32)
#include "sampler/symbol_guess_windows.h"
#elif defined(__linux__) && defined(__x86_64__)
#include "sampler/symbol_guess_linux.h"
#endif

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

std::string allocationHookSummary(const std::vector<AllocationHookCapability> &capabilities)
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

void addSymbolGuessMetadata(ProfileMetadata &meta)
{
#if defined(_WIN32)
    const symbol_guess::windows::BuildStats stats = symbol_guess::windows::currentModuleStats();
    if (!stats.initialized) {
        return;
    }
    meta.extra_platform_metadata["Symbol guess function ranges"] = std::to_string(stats.function_ranges);
    meta.extra_platform_metadata["Symbol guess chained ranges"] = std::to_string(stats.chained_ranges);
    meta.extra_platform_metadata["Symbol guess rejected ranges"] = std::to_string(stats.rejected_ranges);
    meta.extra_platform_metadata["Symbol guess vtables"] = std::to_string(stats.vtables);
    meta.extra_platform_metadata["Symbol guess vtable candidates"] = std::to_string(stats.vtable_candidates);
    meta.extra_platform_metadata["Symbol guess vtable labels"] = std::to_string(stats.vtable_labels);
    meta.extra_platform_metadata["Symbol guess vtable conflicts"] = std::to_string(stats.vtable_conflicts);
    meta.extra_platform_metadata["Symbol guess resolved thunks"] = std::to_string(stats.thunk_resolved);
    meta.extra_platform_metadata["Symbol guess sampled functions"] = std::to_string(stats.sampled_functions);
    meta.extra_platform_metadata["Symbol guess decoded instructions"] = std::to_string(stats.decoded_instructions);
    meta.extra_platform_metadata["Symbol guess string candidates"] = std::to_string(stats.string_candidates);
    meta.extra_platform_metadata["Symbol guess string labels"] = std::to_string(stats.string_labels);
    meta.extra_platform_metadata["Symbol guess shared strings"] = std::to_string(stats.shared_strings);
    meta.extra_platform_metadata["Symbol guess index build microseconds"] = std::to_string(stats.build_microseconds);
    meta.extra_platform_metadata["Symbol guess batch microseconds"] = std::to_string(stats.batch_microseconds);
    meta.extra_platform_metadata["Symbol guess approximate bytes"] = std::to_string(stats.approximate_bytes);
#elif defined(__linux__) && defined(__x86_64__)
    const symbol_guess::linux::BuildStats stats = symbol_guess::linux::currentModuleStats();
    if (!stats.initialized) {
        return;
    }
    meta.extra_platform_metadata["Symbol guess function table entries"] = std::to_string(stats.table_entries);
    meta.extra_platform_metadata["Symbol guess eh_frame records"] = std::to_string(stats.eh_frame_records);
    meta.extra_platform_metadata["Symbol guess function ranges"] = std::to_string(stats.function_ranges);
    meta.extra_platform_metadata["Symbol guess rejected ranges"] = std::to_string(stats.rejected_ranges);
    meta.extra_platform_metadata["Symbol guess duplicate ranges"] = std::to_string(stats.duplicate_ranges);
    meta.extra_platform_metadata["Symbol guess overlap ranges"] = std::to_string(stats.overlap_ranges);
    meta.extra_platform_metadata["Symbol guess unindexed ranges"] = std::to_string(stats.unindexed_ranges);
    meta.extra_platform_metadata["Symbol guess function gaps"] = std::to_string(stats.gap_ranges);
    meta.extra_platform_metadata["Symbol guess function gap bytes"] = std::to_string(stats.gap_bytes);
    meta.extra_platform_metadata["Symbol guess vtables"] = std::to_string(stats.vtables);
    meta.extra_platform_metadata["Symbol guess vtable candidates"] = std::to_string(stats.vtable_candidates);
    meta.extra_platform_metadata["Symbol guess vtable labels"] = std::to_string(stats.vtable_labels);
    meta.extra_platform_metadata["Symbol guess vtable conflicts"] = std::to_string(stats.vtable_conflicts);
    meta.extra_platform_metadata["Symbol guess RTTI types"] = std::to_string(stats.rtti_types);
    meta.extra_platform_metadata["Symbol guess RTTI bases"] = std::to_string(stats.rtti_bases);
    meta.extra_platform_metadata["Symbol guess vtable inheritance resolved"] = std::to_string(stats.vtable_inheritance_resolved);
    meta.extra_platform_metadata["Symbol guess sampled functions"] = std::to_string(stats.sampled_functions);
    meta.extra_platform_metadata["Symbol guess decoded instructions"] = std::to_string(stats.decoded_instructions);
    meta.extra_platform_metadata["Symbol guess string candidates"] = std::to_string(stats.string_candidates);
    meta.extra_platform_metadata["Symbol guess string labels"] = std::to_string(stats.string_labels);
    meta.extra_platform_metadata["Symbol guess string accumulated labels"] = std::to_string(stats.string_accumulated_labels);
    meta.extra_platform_metadata["Symbol guess shared strings"] = std::to_string(stats.shared_strings);
    meta.extra_platform_metadata["Symbol guess thunk candidates"] = std::to_string(stats.thunk_candidates);
    meta.extra_platform_metadata["Symbol guess resolved thunks"] = std::to_string(stats.thunk_resolved);
    meta.extra_platform_metadata["Symbol guess thunk labels"] = std::to_string(stats.thunk_labels);
    meta.extra_platform_metadata["Symbol guess index build microseconds"] = std::to_string(stats.build_microseconds);
    meta.extra_platform_metadata["Symbol guess batch microseconds"] = std::to_string(stats.batch_microseconds);
    meta.extra_platform_metadata["Symbol guess approximate bytes"] = std::to_string(stats.approximate_bytes);
#else
    (void)meta;
#endif
}

} // namespace

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

std::uint64_t Profiler::filteredAllocationSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.filteredSamples() : 0;
}

std::uint64_t Profiler::allocationThreadNameFailures() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.threadNameFailures() : 0;
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

std::size_t Profiler::allocationHookTargetCount() const { return allocation_sampler_.hookTargetCount(); }

bool Profiler::start(const ProfilerOptions &options, std::uint64_t main_tid, std::string &error)
{
    if (running_.load()) {
        error = "profiler is already running";
        return false;
    }

    if (options.regex && options.threads.empty()) {
        error = "--regex requires at least one --thread pattern";
        return false;
    }
    if (std::find(options.threads.begin(), options.threads.end(), "*") != options.threads.end() &&
        (options.regex || options.threads.size() != 1)) {
        error = "--thread * cannot be combined with other thread selectors or --regex";
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
        config.session_seed = main_tid;
        config.only_ticks_over_ms = options.only_ticks_over_ms > 0 ? options.only_ticks_over_ms : 0;
        config.all_threads = options.threads.empty() || (options.threads.size() == 1 && options.threads.front() == "*");
        config.regex_threads = options.regex;
        if (!config.all_threads) {
            config.thread_patterns = options.threads;
        }
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
        config.all_threads = options.threads.size() == 1 && options.threads.front() == "*";
        config.regex_threads = options.regex;
        if (!config.all_threads) {
            config.thread_patterns = options.threads;
        }
        config.only_ticks_over_ms = options.only_ticks_over_ms > 0 ? options.only_ticks_over_ms : 0;
        sampler_.setTarget(main_tid);
        started = sampler_.start(config);
        if (!started) {
            error = sampler_.lastError().empty() ? "the platform stack-capture backend could not be initialized"
                                                 : sampler_.lastError();
        }
    }

    if (!started) {
        return false;
    }

    running_.store(true);
    start_time_ms_ = session_start_ms;
    end_time_ms_ = 0;
    auto_end_time_ms_ =
        options.timeout_seconds > 0 ? start_time_ms_ + static_cast<std::int64_t>(options.timeout_seconds) * 1000 : -1;
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
    const std::int64_t requested_end_time_ms = nowMs();
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
    end_time_ms_ = requested_end_time_ms;
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

std::uint64_t Profiler::activeNumberOfTicks() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.numberOfTicks() : sampler_.numberOfTicks();
}

std::string Profiler::exportData(const ExportContext &ctx) const
{
    ProfileMetadata meta;
    meta.start_time_ms = start_time_ms_;
    meta.end_time_ms = end_time_ms_ > 0 ? end_time_ms_ : nowMs();
    meta.interval = interval_;
    meta.mode = mode_;
    meta.number_of_ticks = static_cast<std::int32_t>(activeNumberOfTicks());
    meta.endstone_version = ctx.endstone_version;
    meta.minecraft_version = ctx.minecraft_version;
    if (mode_ == ProfileMode::Allocation) {
#if defined(_WIN32)
        meta.engine_version = std::string("endstone-spark ") + kVersion + " native-ucrt/funchook";
#elif defined(__linux__)
        meta.engine_version = std::string("endstone-spark ") + kVersion + " native-glibc/elf-import";
#else
        meta.engine_version = std::string("endstone-spark ") + kVersion + " native-allocation";
#endif
    }
    else {
        meta.engine_version = std::string("endstone-spark ") + kVersion;
    }
    meta.comment = !ctx.comment.empty() ? ctx.comment : options_.comment;
    meta.creator_name = options_.creator_name;
    meta.creator_is_player = options_.creator_is_player;
    meta.all_threads = (mode_ == ProfileMode::Allocation && options_.threads.empty()) ||
                       (options_.threads.size() == 1 && options_.threads.front() == "*");
    meta.regex_threads = options_.regex;
    if (meta.regex_threads) {
        meta.thread_patterns = options_.threads;
    }
    meta.ticked = options_.only_ticks_over_ms > 0;
    meta.tick_threshold_ms = options_.only_ticks_over_ms > 0 ? options_.only_ticks_over_ms : 0;

    if (!ctx.bds_executable_sha256.empty()) {
        meta.extra_platform_metadata["BDS executable SHA-256"] = jsonString(ctx.bds_executable_sha256);
    }

    if (mode_ == ProfileMode::Allocation) {
        // Upstream SamplerMetadata has no dedicated native allocation diagnostics.
        // The viewer JSON-parses every map value, so textual values must be encoded
        // as JSON string literals; numbers and booleans are already valid JSON.
#if defined(_WIN32)
        meta.extra_platform_metadata["Allocation backend"] = jsonString("Windows UCRT/funchook");
        meta.extra_platform_metadata["Allocation coverage"] =
            jsonString("process threads reaching hooked UCRT allocation entry points plus "
                       "aligned/base "
                       "and direct process HeapAlloc/HeapReAlloc entry points when available");
#elif defined(__linux__)
        meta.extra_platform_metadata["Allocation backend"] = jsonString("Linux glibc/ELF import slots");
        meta.extra_platform_metadata["Allocation coverage"] =
            jsonString("process threads reaching patched "
                       "malloc/calloc/realloc/reallocarray/aligned_alloc/"
                       "posix_memalign imports in supported loaded ELF modules");
#endif
        meta.extra_platform_metadata["Allocation hook calls (process-wide)"] =
            std::to_string(allocation_sampler_.hookCalls());
        meta.extra_platform_metadata["Allocation successful allocation calls (process-wide)"] =
            std::to_string(allocation_sampler_.successfulAllocationCalls());
        meta.extra_platform_metadata["Allocation sampling points hit (process-wide)"] =
            std::to_string(allocation_sampler_.samplingPoints());
        meta.extra_platform_metadata["Allocation profile samples accepted"] =
            std::to_string(allocation_sampler_.sampleCount());
        meta.extra_platform_metadata["Allocation samples excluded by thread selector"] =
            std::to_string(allocation_sampler_.filteredSamples());
        meta.extra_platform_metadata["Allocation thread name lookup failures"] =
            std::to_string(allocation_sampler_.threadNameFailures());
        meta.extra_platform_metadata["Allocation thread identity cache drops"] =
            std::to_string(allocation_sampler_.threadIdentityCacheDrops());
        meta.extra_platform_metadata["Allocation samples dropped"] =
            std::to_string(allocation_sampler_.droppedSamples());
        meta.extra_platform_metadata["Allocation sample events dropped"] =
            std::to_string(allocation_sampler_.droppedEvents());
        meta.extra_platform_metadata["Allocation tick events dropped"] =
            std::to_string(allocation_sampler_.droppedTickEvents());
        meta.extra_platform_metadata["Allocation tick event capacity"] =
            std::to_string(allocation_sampler_.tickEventCapacity());
        meta.extra_platform_metadata["Allocation sample events enqueued"] =
            std::to_string(allocation_sampler_.enqueuedSamples());
        meta.extra_platform_metadata["Allocation event queue high-water mark"] =
            std::to_string(allocation_sampler_.eventQueueHighWaterMark());
        meta.extra_platform_metadata["Allocation event queue capacity"] =
            std::to_string(allocation_sampler_.eventQueueCapacity());
        meta.extra_platform_metadata["Allocation tracked sampled frees (process-wide)"] =
            std::to_string(allocation_sampler_.freedSamples());
        meta.extra_platform_metadata["Allocation tracked sampled freed bytes (process-wide)"] =
            std::to_string(allocation_sampler_.freedBytes());
        meta.extra_platform_metadata["Allocation tracked live allocations (process-wide)"] =
            std::to_string(allocation_sampler_.liveSamples());
        meta.extra_platform_metadata["Allocation tracked live bytes (process-wide)"] =
            std::to_string(allocation_sampler_.liveBytes());
        meta.extra_platform_metadata["Allocation tracked live peak (process-wide)"] =
            std::to_string(allocation_sampler_.peakLiveSamples());
        meta.extra_platform_metadata["Allocation live index capacity"] =
            std::to_string(allocation_sampler_.liveIndexCapacity());
        meta.extra_platform_metadata["Allocation sampled thread roots"] =
            std::to_string(allocation_sampler_.sampledThreadCount());
        meta.extra_platform_metadata["Allocation thread root capacity"] =
            std::to_string(allocation_sampler_.threadRootCapacity());
        meta.extra_platform_metadata["Allocation overflow threads"] =
            std::to_string(allocation_sampler_.overflowThreadCount());
        meta.extra_platform_metadata["Allocation thread state drops"] =
            std::to_string(allocation_sampler_.threadStateDrops());
        meta.extra_platform_metadata["Allocation hooked modules"] =
            std::to_string(allocation_sampler_.hookedModuleCount());
        meta.extra_platform_metadata["Allocation attributed module entries"] =
            std::to_string(allocation_sampler_.moduleRegistryCount());
        meta.extra_platform_metadata["Allocation attributed module capacity"] =
            std::to_string(allocation_sampler_.moduleRegistryCapacity());
        meta.extra_platform_metadata["Allocation profile node capacity"] =
            std::to_string(allocation_sampler_.profileNodeCapacity());
#if defined(__linux__)
        meta.extra_platform_metadata["Allocation skipped modules"] =
            std::to_string(allocation_sampler_.skippedModuleCount());
        meta.extra_platform_metadata["Allocation failed modules"] =
            std::to_string(allocation_sampler_.failedModuleCount());
#endif
        meta.extra_platform_metadata["Allocation data incomplete"] =
            allocation_sampler_.dataIncomplete() ? "true" : "false";
        meta.extra_platform_metadata["Allocation average tracked lifetime ms (process-wide)"] =
            std::to_string(allocation_sampler_.averageLifetimeMs());
        meta.extra_platform_metadata["Allocation maximum tracked lifetime ms (process-wide)"] =
            std::to_string(allocation_sampler_.maximumLifetimeMs());
        meta.extra_platform_metadata["Allocation lifecycle records dropped"] =
            std::to_string(allocation_sampler_.lifecycleDropped());
        meta.extra_platform_metadata["Allocation profile sampled bytes"] =
            std::to_string(allocation_sampler_.sampledBytes());
        meta.extra_platform_metadata["Allocation observed request bytes (process-wide)"] =
            std::to_string(allocation_sampler_.observedBytes());
        meta.extra_platform_metadata["Allocation interval bytes"] = std::to_string(interval_);
        meta.extra_platform_metadata["Allocation live-only"] = options_.alloc_live_only ? "true" : "false";
        meta.extra_platform_metadata["Allocation thread filter stage"] = jsonString("aggregation");
        meta.extra_platform_metadata["Allocation thread selection"] =
            jsonString(meta.all_threads ? "all" : (meta.regex_threads ? "regex" : "exact-name"));
        if (options_.alloc_live_only) {
            meta.extra_platform_metadata["Allocation analysis"] =
                jsonString("retained sampled allocations at profile stop; candidates "
                           "require repeated growth verification");
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
        meta.extra_platform_metadata["Allocation hook entry points total"] = std::to_string(capabilities.size());
        meta.extra_platform_metadata["Allocation hook entry points covered"] = std::to_string(active + aliases);
        meta.extra_platform_metadata["Allocation hook targets installed"] =
            std::to_string(allocation_sampler_.hookTargetCount());
        meta.extra_platform_metadata["Allocation hook aliases"] = std::to_string(aliases);
        meta.extra_platform_metadata["Allocation hook capabilities"] = jsonString(allocationHookSummary(capabilities));
    }

    meta.platform_stats.present = true;
    meta.platform_stats.player_count = ctx.player_count;
    meta.platform_stats.online_mode = ctx.online_mode;
    meta.platform_stats.uptime_ms = ctx.uptime_ms;
    const ProcessStats process = gatherProcessStats();
    meta.platform_stats.process_mem_present = process.rss_present;
    meta.platform_stats.process_mem_bytes = process.rss_bytes;
    meta.platform_stats.process_virtual_present = process.virtual_present;
    meta.platform_stats.process_virtual_bytes = process.virtual_bytes;

    meta.statistics = ctx.statistics;
    meta.system_stats = ctx.system_stats;
    meta.system_stats.uptime_present = true;
    meta.system_stats.uptime_ms = ctx.uptime_ms;
    meta.system_stats.present = true;
    meta.plugins = ctx.plugins;
    meta.world = ctx.world;
    meta.window_stats = ctx.window_stats;
    meta.extra_platform_metadata["Statistics history available ms"] = std::to_string(ctx.statistics.history_span_ms);

    if (mode_ == ProfileMode::Allocation) {
        std::vector<ThreadTreeView> threads;
        for (const auto &[id, thread] : allocation_sampler_.threadTrees()) {
            threads.push_back({thread.thread_name, &thread.tree});
            if (!meta.all_threads && !meta.regex_threads && id != 0) {
                meta.thread_ids.push_back(static_cast<std::int64_t>(id));
            }
        }
        if (threads.empty()) {
            threads.push_back({meta.thread_name, &allocation_sampler_.tree()});
        }
        std::vector<FrameKey> keys = collectFrameKeys(threads);
        auto resolved = resolveFrames(allocation_sampler_.modules(), keys);
        addSymbolGuessMetadata(meta);
        return buildSamplerData(meta, threads, resolved);
    }

    std::vector<ThreadTreeView> threads;
    for (const auto &[id, thread] : sampler_.threadTrees()) {
        threads.push_back({thread.thread_name, &thread.tree});
        if (!meta.all_threads && !meta.regex_threads) {
            meta.thread_ids.push_back(static_cast<std::int64_t>(id));
        }
    }
    if (threads.empty()) {
        threads.push_back({meta.thread_name, &sampler_.tree()});
    }
    std::vector<FrameKey> keys = collectFrameKeys(threads);
    auto resolved = resolveFrames(sampler_.modules(), keys);
    addSymbolGuessMetadata(meta);
    return buildSamplerData(meta, threads, resolved);
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

} // namespace spark
