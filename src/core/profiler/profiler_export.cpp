#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "core/profiler/native_attribution.h"
#include "core/profiler/profiler.h"
#include "core/profiler/thread_grouper.h"
#include "core/util/monotonic_time.h"
#include "proto/sampler_data.h"
#include "spark_constants.h"
#ifdef _WIN32
#include "native/symbol/symbol_guess_windows.h"
#elif defined(__linux__) && defined(__x86_64__)
#include "native/symbol/symbol_guess_linux.h"
#endif

namespace spark {
namespace {

std::int64_t nowMs()
{
    return monotonicUnixMillis();
}

// spark-viewer parses every extra_platform_metadata value with JSON.parse().
// The protobuf field is map<string, string>, but each string must therefore be
// a complete JSON document rather than arbitrary display text.
std::string jsonString(std::string_view value)
{
    static constexpr char k_hex[] = "0123456789abcdef";

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
                out.push_back(k_hex[(ch >> 4) & 0x0f]);
                out.push_back(k_hex[ch & 0x0f]);
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

// Apply thread grouping and return the merged views plus owned storage for
// merged trees and label strings (must outlive the views).
struct GroupedThreads {
    std::vector<ThreadTreeView> views;
    std::vector<std::unique_ptr<CallTree>> owned_trees;
    std::deque<std::string> owned_labels;
};

GroupedThreads groupThreads(std::vector<std::pair<std::uint64_t, std::pair<std::string, const CallTree *>>> &&input,
                            ThreadGrouperMode mode)
{
    ThreadGrouper grouper(mode);
    // std::map for deterministic group ordering.
    std::map<ThreadGrouper::GroupKey, std::vector<const CallTree *>> groups;
    for (const auto &[tid, p] : input) {
        auto g = grouper.groupKey(tid, p.first);
        groups[g].push_back(p.second);
    }

    GroupedThreads result;
    for (const auto &[g, trees] : groups) {
        if (mode == ThreadGrouperMode::ByName || trees.size() == 1) {
            result.owned_labels.push_back(grouper.label(g.first));
            result.views.push_back({.name = result.owned_labels.back(), .tree = trees.front()});
        }
        else {
            auto merged = std::make_unique<CallTree>();
            for (const CallTree *tree : trees) {
                mergeCallTree(*merged, *tree);
            }
            result.owned_labels.push_back(grouper.label(g.first));
            result.views.push_back({.name = result.owned_labels.back(), .tree = merged.get()});
            result.owned_trees.push_back(std::move(merged));
        }
    }
    return result;
}

void addSymbolGuessMetadata(ProfileMetadata &meta)
{
#ifdef _WIN32
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
    meta.extra_platform_metadata["Symbol guess vtable interior targets"] =
        std::to_string(stats.vtable_interior_target_rejections);
    meta.extra_platform_metadata["Symbol guess thunk interior destinations"] =
        std::to_string(stats.thunk_interior_destination_rejections);
    meta.extra_platform_metadata["Symbol guess RTTI types"] = std::to_string(stats.rtti_types);
    meta.extra_platform_metadata["Symbol guess RTTI bases"] = std::to_string(stats.rtti_bases);
    meta.extra_platform_metadata["Symbol guess vtable inheritance resolved"] =
        std::to_string(stats.vtable_inheritance_resolved);
    meta.extra_platform_metadata["Symbol guess sampled functions"] = std::to_string(stats.sampled_functions);
    meta.extra_platform_metadata["Symbol guess decoded instructions"] = std::to_string(stats.decoded_instructions);
    meta.extra_platform_metadata["Symbol guess string candidates"] = std::to_string(stats.string_candidates);
    meta.extra_platform_metadata["Symbol guess string labels"] = std::to_string(stats.string_labels);
    meta.extra_platform_metadata["Symbol guess string accumulated labels"] =
        std::to_string(stats.string_accumulated_labels);
    meta.extra_platform_metadata["Symbol guess string reference candidates"] =
        std::to_string(stats.string_reference_candidates);
    meta.extra_platform_metadata["Symbol guess string reference potential hits"] =
        std::to_string(stats.string_reference_potential_hits);
    meta.extra_platform_metadata["Symbol guess string reference exact hits"] =
        std::to_string(stats.string_reference_exact_hits);
    meta.extra_platform_metadata["Symbol guess string reference interior rejections"] =
        std::to_string(stats.string_reference_interior_rejections);
    meta.extra_platform_metadata["Symbol guess string reference ambiguities"] =
        std::to_string(stats.string_reference_ambiguities);
    meta.extra_platform_metadata["Symbol guess string reference shared"] =
        std::to_string(stats.string_reference_shared);
    meta.extra_platform_metadata["Symbol guess string reference terminal hits skipped"] =
        std::to_string(stats.string_reference_terminal_hits_skipped);
    meta.extra_platform_metadata["Symbol guess string reference unindexed"] =
        std::to_string(stats.string_reference_unindexed);
    meta.extra_platform_metadata["Symbol guess string reference unreachable"] =
        std::to_string(stats.string_reference_unreachable);
    meta.extra_platform_metadata["Symbol guess string reference overlaps"] =
        std::to_string(stats.string_reference_overlaps);
    meta.extra_platform_metadata["Symbol guess string validation functions"] =
        std::to_string(stats.string_validation_functions);
    meta.extra_platform_metadata["Symbol guess string function byte budget exhausted"] =
        std::to_string(stats.string_function_byte_budget_exhausted);
    meta.extra_platform_metadata["Symbol guess string function instruction budget exhausted"] =
        std::to_string(stats.string_function_instruction_budget_exhausted);
    meta.extra_platform_metadata["Symbol guess string validation budget exhausted"] =
        std::to_string(stats.string_validation_budget_exhausted);
    meta.extra_platform_metadata["Symbol guess string instruction budget exhausted"] =
        std::to_string(stats.string_instruction_budget_exhausted);
    meta.extra_platform_metadata["Symbol guess lambda body labels"] = std::to_string(stats.lambda_body_labels);
    meta.extra_platform_metadata["Symbol guess code pattern labels"] = std::to_string(stats.code_pattern_labels);
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

}  // namespace

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

void Profiler::addNativePluginSources(ProfileMetadata &meta, const ExportContext &ctx,
                                      const std::vector<FrameKey> &keys,
                                      const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved)
{
    std::unordered_map<std::uintptr_t, std::string> by_base;
    for (const NativePluginSource &source : ctx.native_plugin_sources) {
        by_base.emplace(source.module_base, source.source_id);
    }

    std::unordered_set<std::string> conflicts;
    for (const FrameKey &key : keys) {
        if (key.raw_address < key.rva) {
            continue;
        }
        const auto source = by_base.find(static_cast<std::uintptr_t>(key.raw_address - key.rva));
        const auto frame = resolved.find(key);
        if (source == by_base.end() || frame == resolved.end() || frame->second.class_name.empty()) {
            continue;
        }
        const auto [existing, inserted] = meta.class_sources.emplace(frame->second.class_name, source->second);
        if (!inserted && existing->second != source->second) {
            conflicts.insert(frame->second.class_name);
        }
    }
    for (const std::string &class_name : conflicts) {
        meta.class_sources.erase(class_name);
    }
}

std::string Profiler::exportData(const ExportContext &ctx) const
{
    return exportData(ctx, nullptr);
}

std::string Profiler::exportData(const ExportContext &ctx, const AllocationSnapshot *allocation_snapshot) const
{
    ProfileMetadata meta;
    meta.start_time_ms = start_time_ms_;
    meta.end_time_ms = end_time_ms_ > 0 ? end_time_ms_ : nowMs();
    meta.interval = interval_;
    meta.mode = mode_;
    meta.number_of_ticks = static_cast<std::int32_t>(
        allocation_snapshot != nullptr ? allocation_snapshot->number_of_ticks : activeNumberOfTicks());
    meta.endstone_version = ctx.endstone_version;
    meta.minecraft_version = ctx.minecraft_version;
    if (mode_ == ProfileMode::Allocation) {
        meta.engine_version = std::string("endstone-spark ") + kVersion + " " + AllocationSampler::backendId();
    }
    else {
        meta.engine_version = std::string("endstone-spark ") + kVersion;
    }
    meta.comment = !ctx.comment.empty() ? ctx.comment : options_.comment;
    meta.creator_name = options_.creator_name;
    meta.creator_is_player = options_.creator_is_player;
    meta.creator_unique_id = options_.creator_unique_id;
    meta.all_threads = (mode_ == ProfileMode::Allocation && options_.threads.empty()) ||
                       (options_.threads.size() == 1 && options_.threads.front() == "*");
    meta.regex_threads = options_.regex;
    meta.thread_grouper = options_.thread_grouper;
    if (meta.regex_threads) {
        meta.thread_patterns = options_.threads;
    }
    meta.ticked = options_.only_ticks_over_ms > 0;
    meta.tick_threshold_us = options_.only_ticks_over_ms > 0 ? options_.only_ticks_over_ms * 1000 : 0;
    meta.number_of_included_ticks = meta.ticked ? included_ticks_.load(std::memory_order_relaxed) : 0;

    if (!ctx.bds_executable_sha256.empty()) {
        meta.extra_platform_metadata["BDS executable SHA-256"] = jsonString(ctx.bds_executable_sha256);
    }

    if (mode_ == ProfileMode::Allocation) {
        const AllocationDiagnostics allocation_diagnostics = allocation_sampler_.diagnostics();
        // Upstream SamplerMetadata has no dedicated native allocation diagnostics.
        // The viewer JSON-parses every map value, so textual values must be encoded
        // as JSON string literals; numbers and booleans are already valid JSON.
#ifdef _WIN32
        meta.extra_platform_metadata["Allocation backend"] = jsonString(AllocationSampler::backendName());
        meta.extra_platform_metadata["Allocation coverage"] =
            jsonString("process threads reaching hooked UCRT allocation entry points plus "
                       "aligned/base "
                       "and direct process HeapAlloc/HeapReAlloc entry points when available");
#elif defined(__linux__)
        meta.extra_platform_metadata["Allocation backend"] = jsonString(AllocationSampler::backendName());
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
        meta.extra_platform_metadata["Allocation profile samples accepted"] = std::to_string(
            allocation_snapshot != nullptr ? allocation_snapshot->sample_count : allocation_sampler_.sampleCount());
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
            std::to_string(spark::AllocationSampler::tickEventCapacity());
        meta.extra_platform_metadata["Allocation sample events enqueued"] =
            std::to_string(allocation_sampler_.enqueuedSamples());
        meta.extra_platform_metadata["Allocation event queue high-water mark"] =
            std::to_string(allocation_sampler_.eventQueueHighWaterMark());
        meta.extra_platform_metadata["Allocation event queue capacity"] =
            std::to_string(spark::AllocationSampler::eventQueueCapacity());
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
            std::to_string(spark::AllocationSampler::liveIndexCapacity());
        meta.extra_platform_metadata["Allocation live record capacity"] =
            std::to_string(spark::AllocationSampler::liveRecordCapacity());
        meta.extra_platform_metadata["Allocation sampled thread roots"] =
            std::to_string(allocation_snapshot != nullptr ? allocation_snapshot->thread_trees.size()
                                                          : allocation_sampler_.sampledThreadCount());
        meta.extra_platform_metadata["Allocation thread root capacity"] =
            std::to_string(spark::AllocationSampler::threadRootCapacity());
        meta.extra_platform_metadata["Allocation overflow threads"] =
            std::to_string(allocation_sampler_.overflowThreadCount());
        meta.extra_platform_metadata["Allocation thread state drops"] =
            std::to_string(allocation_sampler_.threadStateDrops());
        meta.extra_platform_metadata["Allocation hooked modules"] =
            std::to_string(allocation_sampler_.hookedModuleCount());
        meta.extra_platform_metadata["Allocation attributed module entries"] =
            std::to_string(allocation_sampler_.moduleRegistryCount());
        meta.extra_platform_metadata["Allocation attributed module capacity"] =
            std::to_string(spark::AllocationSampler::moduleRegistryCapacity());
        meta.extra_platform_metadata["Allocation profile node capacity"] =
            std::to_string(spark::AllocationSampler::profileNodeCapacity());
        meta.extra_platform_metadata["Allocation profile time-entry capacity"] =
            std::to_string(spark::AllocationSampler::profileTimeEntryCapacity());
        meta.extra_platform_metadata["Allocation profile storage sample drops"] =
            std::to_string(allocation_sampler_.profileStorageSampleDrops());
        meta.extra_platform_metadata["Allocation profile storage exhausted"] =
            allocation_sampler_.profileStorageExhausted() ? "true" : "false";
        meta.extra_platform_metadata["Allocation pending samples dropped"] =
            std::to_string(allocation_sampler_.pendingSampleDrops());
        meta.extra_platform_metadata["Allocation pending sample capacity"] =
            std::to_string(spark::AllocationSampler::pendingSampleCapacity());
        meta.extra_platform_metadata["Allocation pending capacity drops"] =
            std::to_string(allocation_sampler_.pendingCapacityDrops());
        meta.extra_platform_metadata["Allocation pending stale drops"] =
            std::to_string(allocation_sampler_.pendingStaleDrops());
        meta.extra_platform_metadata["Allocation terminal in-flight tick samples discarded"] =
            std::to_string(allocation_sampler_.terminalInFlightTickSamplesDiscarded());
        meta.extra_platform_metadata["Allocation pending final drops"] =
            std::to_string(allocation_sampler_.pendingFinalDrops());
        meta.extra_platform_metadata["Allocation module overflow frames"] =
            std::to_string(allocation_sampler_.moduleOverflowFrames());
        meta.extra_platform_metadata["Allocation retained history windows"] =
            std::to_string(allocation_sampler_.retainedHistoryWindows());
        meta.extra_platform_metadata["Allocation history samples pruned"] =
            std::to_string(allocation_sampler_.historySamplesPruned());
        meta.extra_platform_metadata["Allocation history bytes pruned"] =
            std::to_string(allocation_sampler_.historyBytesPruned());
        meta.extra_platform_metadata["Allocation history truncated"] =
            allocation_sampler_.historyTruncated() ? "true" : "false";
#ifdef __linux__
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
        meta.extra_platform_metadata["Allocation lock contention records dropped"] =
            std::to_string(allocation_sampler_.contentionDropped());
        meta.extra_platform_metadata["Allocation stop budget truncated records"] =
            std::to_string(allocation_sampler_.drainTruncated());
        meta.extra_platform_metadata["Allocation stop wait timed out"] =
            allocation_sampler_.stopWaitTimedOut() ? "true" : "false";
        meta.extra_platform_metadata["Allocation profile sampled bytes"] = std::to_string(
            allocation_snapshot != nullptr ? allocation_snapshot->sampled_bytes : allocation_sampler_.sampledBytes());
        meta.extra_platform_metadata["Allocation observed request bytes (process-wide)"] =
            std::to_string(allocation_sampler_.observedBytes());
        meta.extra_platform_metadata["Allocation interval bytes"] = std::to_string(interval_);
        meta.extra_platform_metadata["Allocation live-only"] = options_.alloc_live_only ? "true" : "false";
        meta.extra_platform_metadata["Allocation thread filter stage"] = jsonString("aggregation");
        std::string_view thread_selection = "exact-name";
        if (meta.all_threads) {
            thread_selection = "all";
        }
        else if (meta.regex_threads) {
            thread_selection = "regex";
        }
        meta.extra_platform_metadata["Allocation thread selection"] = jsonString(thread_selection);
        if (options_.alloc_live_only) {
            meta.extra_platform_metadata["Allocation analysis"] =
                jsonString("retained sampled allocations at export time; candidates "
                           "require repeated growth verification");
            meta.extra_platform_metadata["Allocation retained average age ms"] =
                std::to_string(allocation_snapshot != nullptr ? allocation_snapshot->retained_average_age_ms
                                                              : allocation_sampler_.retainedAverageAgeMs());
            meta.extra_platform_metadata["Allocation retained maximum age ms"] =
                std::to_string(allocation_snapshot != nullptr ? allocation_snapshot->retained_maximum_age_ms
                                                              : allocation_sampler_.retainedMaximumAgeMs());
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

        meta.extra_platform_metadata["Allocation diagnostics supported"] =
            allocation_diagnostics.supported ? "true" : "false";
        meta.extra_platform_metadata["Allocation diagnostics accounting state"] =
            jsonString(allocationAccountingStateName(allocation_diagnostics.accounting_state));
        meta.extra_platform_metadata["Allocation diagnostics live index capacity"] =
            std::to_string(allocation_diagnostics.live_index_capacity);
        meta.extra_platform_metadata["Allocation diagnostics live record capacity"] =
            std::to_string(allocation_diagnostics.live_record_capacity);
        meta.extra_platform_metadata["Allocation diagnostics drain truncated"] =
            std::to_string(allocation_diagnostics.drain_truncated);
        meta.extra_platform_metadata["Allocation diagnostics drain truncated allocation events"] =
            std::to_string(allocation_diagnostics.drain_truncated_allocation_events);
        meta.extra_platform_metadata["Allocation diagnostics drain truncated thread observation events"] =
            std::to_string(allocation_diagnostics.drain_truncated_thread_observation_events);
        meta.extra_platform_metadata["Allocation diagnostics drain truncated tick events"] =
            std::to_string(allocation_diagnostics.drain_truncated_tick_events);
        meta.extra_platform_metadata["Allocation diagnostics retained allocations skipped"] =
            std::to_string(allocation_diagnostics.retained_allocations_skipped);
        meta.extra_platform_metadata["Allocation diagnostics record pool acquisition failures"] =
            std::to_string(allocation_diagnostics.record_pool_acquisition_failures);
        meta.extra_platform_metadata["Allocation diagnostics insertion contention failures"] =
            std::to_string(allocation_diagnostics.insertion_contention_failures);
        meta.extra_platform_metadata["Allocation diagnostics exhausted insertion probe failures"] =
            std::to_string(allocation_diagnostics.exhausted_insertion_probe_failures);
        meta.extra_platform_metadata["Allocation diagnostics detach contention attempts"] =
            std::to_string(allocation_diagnostics.detach_contention_attempts);
        meta.extra_platform_metadata["Allocation diagnostics processed allocation events"] =
            std::to_string(allocation_diagnostics.processed_allocation_events);
        meta.extra_platform_metadata["Allocation diagnostics processed thread observation events"] =
            std::to_string(allocation_diagnostics.processed_thread_observation_events);
        meta.extra_platform_metadata["Allocation diagnostics processed tick events"] =
            std::to_string(allocation_diagnostics.processed_tick_events);
        meta.extra_platform_metadata["Allocation diagnostics discarded allocation events"] =
            std::to_string(allocation_diagnostics.discarded_allocation_events);
        meta.extra_platform_metadata["Allocation diagnostics discarded thread observation events"] =
            std::to_string(allocation_diagnostics.discarded_thread_observation_events);
        meta.extra_platform_metadata["Allocation diagnostics discarded tick events"] =
            std::to_string(allocation_diagnostics.discarded_tick_events);
        meta.extra_platform_metadata["Allocation diagnostics consumer lifetime ns"] =
            std::to_string(allocation_diagnostics.consumer_lifetime_elapsed_ns);
        meta.extra_platform_metadata["Allocation diagnostics active drain ns"] =
            std::to_string(allocation_diagnostics.active_drain_elapsed_ns);
        meta.extra_platform_metadata["Allocation diagnostics caller final drain ns"] =
            std::to_string(allocation_diagnostics.caller_final_drain_elapsed_ns);
        meta.extra_platform_metadata["Allocation diagnostics caller final drain allocation events"] =
            std::to_string(allocation_diagnostics.caller_final_drain_allocation_events);
        meta.extra_platform_metadata["Allocation diagnostics caller final drain thread observation events"] =
            std::to_string(allocation_diagnostics.caller_final_drain_thread_observation_events);
        meta.extra_platform_metadata["Allocation diagnostics caller final drain tick events"] =
            std::to_string(allocation_diagnostics.caller_final_drain_tick_events);
        meta.extra_platform_metadata["Allocation diagnostics aggregator CPU supported"] =
            allocation_diagnostics.aggregator_cpu_supported ? "true" : "false";
        meta.extra_platform_metadata["Allocation diagnostics aggregator CPU valid"] =
            allocation_diagnostics.aggregator_cpu_valid ? "true" : "false";
        meta.extra_platform_metadata["Allocation diagnostics aggregator CPU read failure"] =
            allocation_diagnostics.aggregator_cpu_read_failure ? "true" : "false";
        meta.extra_platform_metadata["Allocation diagnostics aggregator CPU ns"] =
            std::to_string(allocation_diagnostics.aggregator_cpu_time_ns);
        meta.extra_platform_metadata["Allocation diagnostics module cache supported"] =
            allocation_diagnostics.module_cache_supported ? "true" : "false";
        meta.extra_platform_metadata["Allocation diagnostics module cache hits"] =
            std::to_string(allocation_diagnostics.module_cache_hits);
        meta.extra_platform_metadata["Allocation diagnostics module cache misses"] =
            std::to_string(allocation_diagnostics.module_cache_misses);
        meta.extra_platform_metadata["Allocation diagnostics module cache insertion refusals"] =
            std::to_string(allocation_diagnostics.module_cache_insertion_refusals);
        meta.extra_platform_metadata["Allocation diagnostics module cache size"] =
            std::to_string(allocation_diagnostics.module_cache_size);
        meta.extra_platform_metadata["Allocation diagnostics module cache capacity"] =
            std::to_string(allocation_diagnostics.module_cache_capacity);
    }
    else {
        meta.extra_platform_metadata["Execution samples dropped"] = std::to_string(sampler_.droppedSamples());
        meta.extra_platform_metadata["Execution queue samples dropped"] =
            std::to_string(sampler_.droppedQueueSamples());
        meta.extra_platform_metadata["Execution pending samples dropped"] =
            std::to_string(sampler_.droppedPendingSamples());
        meta.extra_platform_metadata["Execution terminal in-flight tick samples discarded"] =
            std::to_string(sampler_.terminalInFlightTickSamplesDiscarded());
        meta.extra_platform_metadata["Execution profile samples dropped"] =
            std::to_string(sampler_.droppedProfileSamples());
        meta.extra_platform_metadata["Execution tick events dropped"] = std::to_string(sampler_.droppedTickEvents());
        meta.extra_platform_metadata["Execution sample queue capacity"] =
            std::to_string(Sampler::sampleQueueCapacity());
        meta.extra_platform_metadata["Execution tick event capacity"] = std::to_string(Sampler::tickQueueCapacity());
        meta.extra_platform_metadata["Execution module entries"] = std::to_string(sampler_.modules().size());
        meta.extra_platform_metadata["Execution module capacity"] = std::to_string(Sampler::moduleCapacity());
        meta.extra_platform_metadata["Execution module overflow frames"] =
            std::to_string(sampler_.moduleOverflowFrames());
        meta.extra_platform_metadata["Execution sampled thread roots"] = std::to_string(sampler_.threadTrees().size());
        meta.extra_platform_metadata["Execution thread root capacity"] = std::to_string(Sampler::threadRootCapacity());
        meta.extra_platform_metadata["Execution overflow thread samples"] =
            std::to_string(sampler_.overflowThreadSamples());
        meta.extra_platform_metadata["Execution pending sample capacity"] =
            std::to_string(Sampler::pendingSampleCapacity());
        meta.extra_platform_metadata["Execution profile node capacity"] =
            std::to_string(Sampler::profileNodeCapacity());
        meta.extra_platform_metadata["Execution profile time entry capacity"] =
            std::to_string(Sampler::profileTimeEntryCapacity());
        meta.extra_platform_metadata["Execution profile storage exhausted"] =
            sampler_.profileStorageExhausted() ? "true" : "false";
        meta.extra_platform_metadata["Execution retained history windows"] =
            std::to_string(sampler_.retainedHistoryWindows());
        meta.extra_platform_metadata["Execution history samples pruned"] =
            std::to_string(sampler_.historySamplesPruned());
        meta.extra_platform_metadata["Execution history truncated"] = sampler_.historyTruncated() ? "true" : "false";
        meta.extra_platform_metadata["Execution data incomplete"] = sampler_.dataIncomplete() ? "true" : "false";
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
    meta.metrics = ctx.metrics;
    meta.system_stats = ctx.system_stats;
    meta.system_stats.uptime_present = true;
    meta.system_stats.uptime_ms = ctx.uptime_ms;
    meta.system_stats.present = true;
    meta.plugins = ctx.plugins;
    meta.world = ctx.world;
    meta.server_configurations = ctx.server_configurations;
    meta.window_stats = ctx.window_stats;
    meta.socket_channel_info_proto = ctx.socket_channel_info_proto;
    meta.extra_platform_metadata["Statistics history available ms"] = std::to_string(ctx.statistics.history_span_ms);

    // Populate ping rolling average if samples were collected.
    if (!ctx.ping_samples.empty()) {
        PingRollingAverage temp(PingStatistics::kWindowSize);
        for (int v : ctx.ping_samples) {
            temp.add(v);
        }
        meta.platform_stats.ping_present = true;
        meta.platform_stats.ping_mean = temp.mean();
        meta.platform_stats.ping_max = static_cast<double>(temp.max());
        meta.platform_stats.ping_min = static_cast<double>(temp.min());
        meta.platform_stats.ping_median = static_cast<double>(temp.median());
        meta.platform_stats.ping_p95 = static_cast<double>(temp.percentile95th());
    }

    // Populate network rolling averages if snapshots were collected.
    if (!ctx.net_snapshots.empty()) {
        meta.system_stats.net_present = true;
        meta.system_stats.net_averages = ctx.net_snapshots;
    }

    if (mode_ == ProfileMode::Allocation) {
        const auto &thread_trees =
            allocation_snapshot != nullptr ? allocation_snapshot->thread_trees : allocation_sampler_.threadTrees();
        const CallTree &tree = allocation_snapshot != nullptr ? allocation_snapshot->tree : allocation_sampler_.tree();
        const ModuleTable &modules =
            allocation_snapshot != nullptr ? allocation_snapshot->modules : allocation_sampler_.modules();
        std::vector<std::pair<std::uint64_t, std::pair<std::string, const CallTree *>>> input;
        for (const auto &[id, thread] : thread_trees) {
            input.emplace_back(id, std::make_pair(thread.thread_name, &thread.tree));
            if (!meta.all_threads && !meta.regex_threads && id != 0) {
                meta.thread_ids.push_back(static_cast<std::int64_t>(id));
            }
        }
        if (input.empty()) {
            input.emplace_back(0, std::make_pair(meta.thread_name, &tree));
        }
        auto [threads, owned_trees, owned_labels] = groupThreads(std::move(input), options_.thread_grouper);
        std::vector<FrameKey> keys = collectFrameKeys(threads);
        auto resolved = resolveFrames(modules, keys);
        addNativePluginSources(meta, ctx, keys, resolved);
        addSymbolGuessMetadata(meta);
        return buildSamplerData(meta, threads, resolved);
    }

    std::vector<std::pair<std::uint64_t, std::pair<std::string, const CallTree *>>> input;
    for (const auto &[id, thread] : sampler_.threadTrees()) {
        input.emplace_back(id, std::make_pair(thread.thread_name, &thread.tree));
        if (!meta.all_threads && !meta.regex_threads) {
            meta.thread_ids.push_back(static_cast<std::int64_t>(id));
        }
    }
    if (input.empty()) {
        input.emplace_back(0, std::make_pair(meta.thread_name, &sampler_.tree()));
    }
    auto [threads, owned_trees, owned_labels] = groupThreads(std::move(input), options_.thread_grouper);
    std::vector<FrameKey> keys = collectFrameKeys(threads);
    auto resolved = resolveFrames(sampler_.modules(), keys);
    filterExecutionTrees(threads, owned_trees, resolved);
    keys = collectFrameKeys(threads);
    addNativePluginSources(meta, ctx, keys, resolved);
    addSymbolGuessMetadata(meta);
    return buildSamplerData(meta, threads, resolved);
}

}  // namespace spark
