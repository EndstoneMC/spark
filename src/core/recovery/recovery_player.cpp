#include "core/recovery/recovery_player.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <link.h>
#endif

#include "core/profiler/profile_mode.h"
#include "core/profiler/thread_grouper.h"
#include "core/recovery/journal_reader.h"
#include "native/sampler/call_tree.h"
#include "native/sampler/sampler.h"
#include "native/sampler/types.h"
#include "native/symbol/symbolicate.h"
#include "profiling_window.h"
#include "proto/sampler_data.h"
#include "spark_constants.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

#ifdef _WIN32

std::uint64_t moduleBaseForPath(const std::string &path)
{
    HMODULE h = GetModuleHandleA(path.c_str());
    return h ? reinterpret_cast<std::uint64_t>(h) : 0;
}

#else

struct ModuleBaseFinder {
    std::string target;
    std::uint64_t base = 0;
};

int phdrCallback(struct dl_phdr_info *info, size_t /*size*/, void *data)
{
    auto *finder = static_cast<ModuleBaseFinder *>(data);
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0' && std::string(info->dlpi_name) == finder->target) {
        finder->base = static_cast<std::uint64_t>(info->dlpi_addr);
        return 1;
    }
    return 0;
}

std::uint64_t moduleBaseForPath(const std::string &path)
{
    ModuleBaseFinder finder{.target = path, .base = 0};
    dl_iterate_phdr(phdrCallback, &finder);
    return finder.base;
}

#endif

}  // namespace

RecoveredProfile RecoveryPlayer::replay(const std::filesystem::path &directory)
{
    RecoveredProfile result;

    JournalReadResult journal = JournalReader::readSession(directory);
    if (!journal.valid) {
        result.error = "no valid journal found";
        return result;
    }

    result.session_start_ms = static_cast<std::int64_t>(journal.session_id);
    result.has_clean_end = journal.has_clean_end;
    result.corrupt_records = journal.corrupt_records;
    result.truncated_records = journal.truncated_records;

    // Clean-end sessions were already exported through the normal path; the
    // journal is just leftover state.  Skip the expensive replay/symbolication
    // and let the caller clean up.
    if (journal.has_clean_end) {
        result.valid = true;
        return result;
    }

    if (journal.records.empty()) {
        result.error = "journal contains no records";
        return result;
    }

    if (journal.duplicate_sequences) {
        result.error = "journal contains duplicate record sequences";
        return result;
    }

    // Head-truncated journal without a valid metadata snapshot: rotation
    // deleted the early segments that carry ModuleDef/ThreadDef/SessionConfig.
    // Replaying the retained tail would reference module IDs whose definitions
    // are gone.  Treat as incomplete.
    if (journal.head_truncated) {
        result.error = "journal is head-truncated (missing early segments)";
        return result;
    }

    // Build module table from the metadata snapshot (if present) and ModuleDef
    // records.  The snapshot supplies definitions from pruned segments; the
    // retained segment records supply definitions added after the snapshot.
    // Recorded module IDs may not match local intern() IDs if any ModuleDef
    // was lost, so build an explicit recorded->local remap and validate every
    // sample against it.
    ModuleTable modules;
    std::unordered_map<ModuleId, ModuleId> module_remap;
    std::unordered_map<ModuleId, std::uint64_t> module_bases;

    if (journal.metadata_snapshot && journal.metadata_snapshot->valid) {
        for (const auto &[recorded_id, path] : journal.metadata_snapshot->modules) {
            if (module_remap.contains(recorded_id)) {
                continue;
            }
            ModuleId local_id = modules.intern(path);
            module_remap[recorded_id] = local_id;
            std::uint64_t base = moduleBaseForPath(path);
            if (base != 0) {
                module_bases[local_id] = base;
            }
        }
    }

    for (const auto &rec : journal.records) {
        if (rec.type != RecordType::ModuleDef) {
            continue;
        }
        std::uint32_t recorded_id;
        std::string path;
        if (rec.asModuleDef(recorded_id, path)) {
            if (module_remap.contains(recorded_id)) {
                continue;
            }
            ModuleId local_id = modules.intern(path);
            module_remap[recorded_id] = local_id;
            std::uint64_t base = moduleBaseForPath(path);
            if (base != 0) {
                module_bases[local_id] = base;
            }
        }
    }

    // Replay samples into per-thread call trees.
    CallTree global_tree;
    std::map<std::uint64_t, ThreadCallTree> thread_trees;
    std::unordered_map<std::uint64_t, std::string> thread_names;
    std::optional<std::uint64_t> min_tick_id;
    std::optional<std::uint64_t> max_tick_id;
    std::optional<std::int32_t> min_window;
    std::uint64_t sample_count = 0;

    if (journal.metadata_snapshot && journal.metadata_snapshot->valid) {
        for (const auto &def : journal.metadata_snapshot->threads) {
            thread_names[def.thread_id] = def.name;
        }
    }

    for (const auto &rec : journal.records) {
        if (rec.type == RecordType::ThreadDef) {
            std::uint64_t thread_id;
            std::uint64_t os_thread_id;
            std::string name;
            if (rec.asThreadDef(thread_id, os_thread_id, name)) {
                thread_names[thread_id] = std::move(name);
            }
        }
        else if (rec.type == RecordType::Sample) {
            std::uint64_t thread_id;
            std::uint64_t tick_id;
            std::uint64_t weight;
            std::int32_t window;
            std::vector<FrameKey> frames;
            if (rec.asSample(thread_id, tick_id, window, weight, frames)) {
                min_tick_id = min_tick_id ? std::min(*min_tick_id, tick_id) : tick_id;
                max_tick_id = max_tick_id ? std::max(*max_tick_id, tick_id) : tick_id;
                min_window = min_window ? std::min(*min_window, window) : window;
            }
        }
        else if (rec.type == RecordType::TickEvent) {
            std::uint64_t tick_id;
            double mspt;
            if (rec.asTickEvent(tick_id, mspt)) {
                min_tick_id = min_tick_id ? std::min(*min_tick_id, tick_id) : tick_id;
                max_tick_id = max_tick_id ? std::max(*max_tick_id, tick_id) : tick_id;
            }
        }
    }

    const std::uint64_t retained_tick_start = min_tick_id.value_or(0);
    const std::int32_t retained_window_start = min_window.value_or(0);
    result.session_start_ms += static_cast<std::int64_t>(retained_window_start) * profiling_window::kSizeMs;

    // TickEvent records don't carry a window field, so we build a tick_id ->
    // window map from Sample records (which do) and use it to assign each tick
    // to the correct per-window statistics bucket.
    std::map<std::uint64_t, std::int32_t> tick_to_window;
    struct TickEventEntry {
        std::uint64_t tick_id;
        double mspt;
    };
    std::vector<TickEventEntry> tick_events;

    for (const auto &rec : journal.records) {
        if (rec.type == RecordType::Sample) {
            std::uint64_t thread_id, tick_id, weight;
            std::int32_t window;
            std::vector<FrameKey> frames;
            if (!rec.asSample(thread_id, tick_id, window, weight, frames)) {
                continue;
            }

            window -= retained_window_start;
            tick_to_window[tick_id] = window;
            for (auto &frame : frames) {
                auto remap_it = module_remap.find(frame.module);
                if (remap_it == module_remap.end()) {
                    result.error = "sample references missing module id " + std::to_string(frame.module);
                    return result;
                }
                frame.module = remap_it->second;
                auto base_it = module_bases.find(frame.module);
                if (base_it != module_bases.end()) {
                    frame.raw_address = base_it->second + frame.rva;
                }
            }

            global_tree.log(frames, window, weight);
            auto [it, inserted] = thread_trees.try_emplace(thread_id);
            if (inserted) {
                it->second.thread_id = thread_id;
                if (auto name = thread_names.find(thread_id); name != thread_names.end()) {
                    it->second.thread_name = name->second;
                }
            }
            it->second.tree.log(frames, window, weight);
            ++sample_count;
        }
        else if (rec.type == RecordType::TickEvent) {
            std::uint64_t tick_id;
            double mspt;
            if (rec.asTickEvent(tick_id, mspt)) {
                tick_events.push_back({.tick_id = tick_id, .mspt = mspt});
            }
        }
    }

    result.sample_count = sample_count;
    result.thread_count = thread_trees.size();
    result.tick_count = max_tick_id ? *max_tick_id - retained_tick_start + 1 : 0;

    // Reconstruct per-window tick statistics; the viewer divides total time by per-window ticks.
    struct WindowAccumulator {
        int ticks = 0;
        std::vector<double> mspts;
        double mspt_max = 0.0;
    };
    std::map<std::int32_t, WindowAccumulator> window_acc;
    for (const auto &te : tick_events) {
        std::int32_t window = 0;
        auto it = tick_to_window.find(te.tick_id);
        if (it != tick_to_window.end()) {
            window = it->second;
        }
        else {
            auto lower = tick_to_window.lower_bound(te.tick_id);
            if (lower != tick_to_window.begin()) {
                --lower;
                window = lower->second;
            }
        }
        WindowAccumulator &acc = window_acc[window];
        acc.ticks += 1;
        acc.mspts.push_back(te.mspt);
        acc.mspt_max = std::max(te.mspt, acc.mspt_max);
    }

    std::map<std::int32_t, WindowStats> window_stats;
    for (const auto &[window, acc] : window_acc) {
        WindowStats ws;
        ws.ticks_present = true;
        ws.ticks = acc.ticks;
        ws.mspt_present = true;
        ws.mspt_max = acc.mspt_max;
        if (!acc.mspts.empty()) {
            std::vector<double> sorted = acc.mspts;
            std::ranges::sort(sorted);
            ws.mspt_median = sorted[sorted.size() / 2];
        }
        ws.start_time_ms = result.session_start_ms + static_cast<std::int64_t>(window) * profiling_window::kSizeMs;
        ws.end_time_ms = ws.start_time_ms + profiling_window::kSizeMs;
        ws.duration_ms = static_cast<int>(profiling_window::kSizeMs);
        ws.tps_present = true;
        ws.tps = static_cast<double>(acc.ticks) * 1000.0 / static_cast<double>(ws.duration_ms);
        window_stats[window] = ws;
    }

    if (sample_count == 0) {
        result.error = "journal contains no samples";
        return result;
    }

    // Build profile metadata from the session config record.
    const SessionConfig &sc = journal.session_config;
    if (sc.present && sc.live_only) {
        result.error = "allocation live-only recovery is not supported";
        return result;
    }
    ProfileMetadata meta;
    meta.start_time_ms = result.session_start_ms;
    meta.end_time_ms = nowMs();
    meta.interval = sc.present ? static_cast<std::int32_t>(sc.interval_us) : 4000;
    meta.mode = sc.present && sc.profile_type == 1 ? ProfileMode::Allocation : ProfileMode::Execution;
    meta.number_of_ticks = static_cast<std::int32_t>(result.tick_count);
    meta.engine_version = std::string("endstone-spark ") + kVersion + " (crash recovery)";
    meta.creator_name = sc.present ? sc.creator_name : "crash recovery";
    meta.creator_is_player = sc.present && sc.creator_is_player;
    meta.comment = sc.present && !sc.comment.empty() ? sc.comment + " [recovered from crash journal]"
                                                     : "Recovered from crash journal";
    meta.all_threads = sc.present && sc.all_threads;
    meta.regex_threads = sc.present && sc.regex_threads;
    meta.thread_grouper = sc.present ? static_cast<ThreadGrouperMode>(sc.thread_grouper) : ThreadGrouperMode::ByPool;
    if (sc.present && sc.regex_threads) {
        meta.thread_patterns = sc.thread_patterns;
    }
    meta.ticked = sc.present && sc.only_ticks_over_ms > 0;
    meta.tick_threshold_ms = sc.present && sc.only_ticks_over_ms > 0 ? sc.only_ticks_over_ms : 0;
    meta.window_stats = window_stats;

    // Collect thread views for serialization.
    std::vector<std::pair<std::uint64_t, std::pair<std::string, const CallTree *>>> input;
    for (const auto &[id, thread] : thread_trees) {
        input.emplace_back(id, std::make_pair(thread.thread_name, &thread.tree));
        if (!meta.all_threads && !meta.regex_threads) {
            meta.thread_ids.push_back(static_cast<std::int64_t>(id));
        }
    }
    if (input.empty()) {
        input.emplace_back(0, std::make_pair(meta.thread_name, &global_tree));
    }

    // Group threads (matching the normal export path).
    ThreadGrouper grouper(meta.thread_grouper);
    std::map<std::string, std::vector<const CallTree *>> groups;
    for (const auto &[tid, p] : input) {
        std::string g = grouper.group(tid, p.first);
        groups[g].push_back(p.second);
    }

    std::vector<ThreadTreeView> views;
    std::vector<std::unique_ptr<CallTree>> owned_trees;
    std::deque<std::string> owned_labels;
    for (const auto &[g, trees] : groups) {
        if (meta.thread_grouper == ThreadGrouperMode::ByName || trees.size() == 1) {
            owned_labels.push_back(grouper.label(g));
            views.push_back({.name = owned_labels.back(), .tree = trees.front()});
        }
        else {
            auto merged = std::make_unique<CallTree>();
            for (const CallTree *tree : trees) {
                mergeCallTree(*merged, *tree);
            }
            owned_labels.push_back(grouper.label(g));
            views.push_back({.name = owned_labels.back(), .tree = merged.get()});
            owned_trees.push_back(std::move(merged));
        }
    }

    // Resolve symbols and serialize.  Wrap the platform-specific symbol
    // resolution and protobuf build so a corrupt or unexpected journal can
    // never throw out of replay().
    try {
        std::vector<FrameKey> keys = collectFrameKeys(views);
        auto resolved = resolveFrames(modules, keys);
        result.serialized_proto = buildSamplerData(meta, views, resolved);
        result.valid = true;
    }
    catch (const std::exception &e) {
        result.error = std::string("recovery replay failed: ") + e.what();
    }
    catch (...) {
        result.error = "recovery replay failed: unknown error";
    }
    return result;
}

}  // namespace spark
