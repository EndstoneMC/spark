#include "core/recovery/recovery_player.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <link.h>
#endif

#include "core/profiler/profile_mode.h"
#include "core/profiler/thread_grouper.h"
#include "core/recovery/journal_reader.h"
#include "native/sampler/call_tree.h"
#include "native/sampler/sampler.h"
#include "native/sampler/types.h"
#include "native/symbol/symbolicate.h"
#include "proto/sampler_data.h"
#include "spark_constants.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

#if defined(_WIN32)

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
    if (info->dlpi_name && info->dlpi_name[0] != '\0' &&
        std::string(info->dlpi_name) == finder->target) {
        finder->base = static_cast<std::uint64_t>(info->dlpi_addr);
        return 1;
    }
    return 0;
}

std::uint64_t moduleBaseForPath(const std::string &path)
{
    ModuleBaseFinder finder{path, 0};
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
    if (journal.records.empty()) {
        result.error = "journal contains no records";
        return result;
    }

    result.session_start_ms = static_cast<std::int64_t>(journal.session_id);
    result.has_clean_end = journal.has_clean_end;
    result.corrupt_records = journal.corrupt_records;
    result.truncated_records = journal.truncated_records;

    // Build module table from ModuleDef records.  Records are journaled in
    // first-intern order, so replaying intern() in journal order reproduces
    // the original module IDs.
    ModuleTable modules;
    std::unordered_map<ModuleId, std::uint64_t> module_bases;
    for (const auto &rec : journal.records) {
        if (rec.type != RecordType::ModuleDef) continue;
        std::uint32_t module_id;
        std::string path;
        if (rec.asModuleDef(module_id, path)) {
            modules.intern(path);
            std::uint64_t base = moduleBaseForPath(path);
            if (base != 0) {
                module_bases[module_id] = base;
            }
        }
    }

    // Replay samples into per-thread call trees.
    CallTree global_tree;
    std::map<std::uint64_t, ThreadCallTree> thread_trees;
    std::uint64_t max_tick_id = 0;
    std::uint64_t sample_count = 0;

    for (const auto &rec : journal.records) {
        if (rec.type == RecordType::Sample) {
            std::uint64_t thread_id, tick_id, weight;
            std::int32_t window;
            std::vector<FrameKey> frames;
            if (!rec.asSample(thread_id, tick_id, window, weight, frames)) continue;

            for (auto &frame : frames) {
                auto it = module_bases.find(frame.module);
                if (it != module_bases.end()) {
                    frame.raw_address = it->second + frame.rva;
                }
            }

            global_tree.log(frames, window, weight);
            auto [it, inserted] = thread_trees.try_emplace(thread_id);
            if (inserted) {
                it->second.thread_id = thread_id;
            }
            it->second.tree.log(frames, window, weight);
            ++sample_count;
            if (tick_id > max_tick_id) max_tick_id = tick_id;
        }
        else if (rec.type == RecordType::ThreadDef) {
            std::uint64_t thread_id, os_thread_id;
            std::string name;
            if (rec.asThreadDef(thread_id, os_thread_id, name)) {
                auto [it, inserted] = thread_trees.try_emplace(thread_id);
                if (inserted) {
                    it->second.thread_id = thread_id;
                }
                it->second.thread_name = name;
            }
        }
        else if (rec.type == RecordType::TickEvent) {
            std::uint64_t tick_id;
            double mspt;
            if (rec.asTickEvent(tick_id, mspt) && tick_id > max_tick_id) {
                max_tick_id = tick_id;
            }
        }
    }

    result.sample_count = sample_count;
    result.thread_count = thread_trees.size();
    result.tick_count = max_tick_id > 0 ? max_tick_id + 1 : 0;

    if (sample_count == 0) {
        result.error = "journal contains no samples";
        return result;
    }

    // Build profile metadata from the session config record.
    const SessionConfig &sc = journal.session_config;
    ProfileMetadata meta;
    meta.start_time_ms = result.session_start_ms;
    meta.end_time_ms = nowMs();
    meta.interval = sc.present ? static_cast<std::int32_t>(sc.interval_us) : 4000;
    meta.mode = sc.present && sc.profile_type == 1
                    ? ProfileMode::Allocation
                    : ProfileMode::Execution;
    meta.number_of_ticks = static_cast<std::int32_t>(result.tick_count);
    meta.engine_version = std::string("endstone-spark ") + kVersion + " (crash recovery)";
    meta.creator_name = sc.present ? sc.creator_name : "crash recovery";
    meta.creator_is_player = sc.present && sc.creator_is_player;
    meta.comment = sc.present && !sc.comment.empty()
                       ? sc.comment + " [recovered from crash journal]"
                       : "Recovered from crash journal";
    meta.all_threads = sc.present && sc.all_threads;
    meta.regex_threads = sc.present && sc.regex_threads;
    meta.thread_grouper = sc.present
                              ? static_cast<ThreadGrouperMode>(sc.thread_grouper)
                              : ThreadGrouperMode::ByPool;
    if (sc.present && sc.regex_threads) {
        meta.thread_patterns = sc.thread_patterns;
    }
    meta.ticked = sc.present && sc.only_ticks_over_ms > 0;
    meta.tick_threshold_ms = sc.present && sc.only_ticks_over_ms > 0
                                 ? sc.only_ticks_over_ms
                                 : 0;

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
            views.push_back({owned_labels.back(), trees.front()});
        } else {
            auto merged = std::make_unique<CallTree>();
            for (const CallTree *tree : trees) {
                mergeCallTree(*merged, *tree);
            }
            owned_labels.push_back(grouper.label(g));
            views.push_back({owned_labels.back(), merged.get()});
            owned_trees.push_back(std::move(merged));
        }
    }

    // Resolve symbols.
    std::vector<FrameKey> keys = collectFrameKeys(views);
    auto resolved = resolveFrames(modules, keys);

    // Serialize.
    try {
        result.serialized_proto = buildSamplerData(meta, views, resolved);
        result.valid = true;
    }
    catch (const std::exception &e) {
        result.error = std::string("serialization failed: ") + e.what();
    }
    return result;
}

}  // namespace spark
