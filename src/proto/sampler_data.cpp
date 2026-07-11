#include "proto/sampler_data.h"

#include <algorithm>
#include <set>
#include <unordered_set>

#include "proto/proto_writer.h"
#include "spark_constants.h"

namespace spark {

namespace {

std::uint64_t nodeTotal(const CallTree::Node &n)
{
    std::uint64_t total = 0;
    for (const auto &[window, count] : n.times) {
        total += count;
    }
    return total;
}

std::vector<double> alignTimes(const std::map<std::int32_t, std::uint64_t> &times,
                               const std::vector<std::int32_t> &windows, std::int32_t interval_us)
{
    std::vector<double> out(windows.size(), 0.0);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        auto it = times.find(windows[i]);
        if (it != times.end()) {
            out[i] = static_cast<double>(it->second) * static_cast<double>(interval_us) / 1000.0;
        }
    }
    return out;
}

std::vector<const CallTree::Node *> sortedChildren(const CallTree::Node &node)
{
    std::vector<const CallTree::Node *> kids;
    kids.reserve(node.children.size());
    for (const auto &[key, child] : node.children) {
        kids.push_back(child.get());
    }
    std::sort(kids.begin(), kids.end(), [](const CallTree::Node *a, const CallTree::Node *b) {
        std::uint64_t ta = nodeTotal(*a);
        std::uint64_t tb = nodeTotal(*b);
        return ta != tb ? ta > tb : a->key.rva < b->key.rva;
    });
    return kids;
}

// Post-order flatten: append children first, then this node; return this node's index.
int emitNode(const CallTree::Node *node, const std::vector<std::int32_t> &windows, std::int32_t interval_us,
             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved, std::vector<std::string> &flat)
{
    std::vector<std::int32_t> child_refs;
    for (const CallTree::Node *kid : sortedChildren(*node)) {
        child_refs.push_back(emitNode(kid, windows, interval_us, resolved, flat));
    }

    std::string bytes;
    ProtoWriter w(bytes);
    auto it = resolved.find(node->key);
    if (it != resolved.end()) {
        w.string(3, it->second.class_name);
        w.string(4, it->second.method_name);
        if (it->second.line >= 0) {
            w.int32(6, it->second.line);
        }
        if (!it->second.method_desc.empty()) {
            w.string(7, it->second.method_desc);
        }
    }
    w.packedDouble(8, alignTimes(node->times, windows, interval_us));
    w.packedInt32(9, child_refs);

    flat.push_back(std::move(bytes));
    return static_cast<int>(flat.size()) - 1;
}

std::string buildMetadata(const ProfileMetadata &m)
{
    std::string out;
    ProtoWriter w(out);

    // creator (1): CommandSenderMetadata { type, name }
    {
        std::string c;
        ProtoWriter cw(c);
        cw.varint(1, m.creator_is_player ? 1 : 0);
        cw.string(2, m.creator_name);
        w.message(1, c);
    }
    w.int64(2, m.start_time_ms);
    w.int32(3, m.interval_us);
    // thread_dumper (4): { type = SPECIFIC }
    {
        std::string t;
        ProtoWriter tw(t);
        tw.varint(1, 1);
        w.message(4, t);
    }
    // data_aggregator (5): { type, thread_grouper = BY_NAME, tick_length_threshold }
    {
        std::string d;
        ProtoWriter dw(d);
        dw.varint(1, m.ticked ? 1 : 0);
        dw.varint(2, 0);
        if (m.ticked) {
            dw.int64(3, m.tick_threshold_ms);
        }
        w.message(5, d);
    }
    if (!m.comment.empty()) {
        w.string(6, m.comment);
    }
    // platform_metadata (7)
    {
        std::string p;
        ProtoWriter pw(p);
        pw.varint(1, 0);  // type = SERVER
        pw.string(2, "Endstone");
        pw.string(3, m.endstone_version);
        if (!m.minecraft_version.empty()) {
            pw.string(4, m.minecraft_version);
        }
        pw.int32(7, kSparkFormatVersion);  // spark_version (gates viewer feature support)
        pw.string(8, "Endstone");          // brand
        w.message(7, p);
    }
    w.int64(11, m.end_time_ms);
    w.int32(12, m.number_of_ticks);
    w.varint(15, 0);  // sampler_mode = EXECUTION
    w.varint(16, 1);  // sampler_engine = ASYNC
    if (!m.engine_version.empty()) {
        w.string(17, m.engine_version);
    }

    // platform_statistics (8)
    if (m.platform_stats.present) {
        const PlatformStats &p = m.platform_stats;
        std::string ps;
        ProtoWriter pw(ps);
        {  // memory (1) -> heap (1) MemoryUsage { used, committed }  (viewer renders this widget unconditionally)
            std::string heap;
            ProtoWriter hw(heap);
            hw.int64(1, p.process_mem_bytes);  // used = VmRSS (matches /status "Used memory")
            // committed = the widget's denominator = VmSize, so the "process" memory widget
            // matches Endstone's /status ("Used memory" = RSS, "Total memory" = VmSize).
            hw.int64(2, p.process_virtual_bytes > 0 ? p.process_virtual_bytes : p.process_mem_bytes);
            std::string mem;
            ProtoWriter mw(mem);
            mw.message(1, heap);
            pw.message(1, mem);
        }
        pw.int64(3, p.uptime_ms);
        {  // tps (4)
            std::string t;
            ProtoWriter tw(t);
            tw.real(1, p.tps);
            tw.real(2, p.tps);
            tw.real(3, p.tps);
            tw.int32(4, p.target_tps);
            pw.message(4, t);
        }
        {  // mspt (5): RollingAverageValues { mean, max, min, median, p95 } for last1m/last5m
            std::string rav;
            ProtoWriter rw(rav);
            rw.real(1, p.mspt);
            rw.real(2, p.mspt_max);
            rw.real(3, p.mspt);
            rw.real(4, p.mspt);
            rw.real(5, p.mspt_max);
            std::string t;
            ProtoWriter tw(t);
            tw.message(1, rav);
            tw.message(2, rav);
            tw.int32(3, p.max_ideal_mspt);
            pw.message(5, t);
        }
        if (p.player_count >= 0) {
            pw.int64(7, p.player_count);
        }
        if (p.online_mode > 0) {
            pw.varint(9, static_cast<std::uint64_t>(p.online_mode));
        }
        if (m.world.present) {  // world (8): WorldStatistics { total_entities, entity_counts, worlds }
            std::string ws;
            ProtoWriter wsw(ws);
            wsw.int32(1, m.world.total_entities);
            for (const auto &[type, count] : m.world.entity_counts) {  // entity_counts (2) map<string,int32>
                std::string ec;
                ProtoWriter ecw(ec);
                ecw.string(1, type);
                ecw.int32(2, count);
                wsw.message(2, ec);
            }
            for (const WorldEntry &we : m.world.worlds) {  // worlds (3) repeated World { name, total_entities }
                std::string world;
                ProtoWriter worldw(world);
                worldw.string(1, we.name);
                worldw.int32(2, we.total_entities);
                wsw.message(3, world);
            }
            pw.message(8, ws);
        }
        w.message(8, ps);
    }

    // system_statistics (9)
    if (m.system_stats.present) {
        const SystemStats &s = m.system_stats;
        std::string ss;
        ProtoWriter sw(ss);
        {  // cpu (1)
            std::string c;
            ProtoWriter cw(c);
            cw.int32(1, s.cpu_threads);
            std::string up;
            ProtoWriter upw(up);
            upw.real(1, s.cpu_process);
            upw.real(2, s.cpu_process);
            cw.message(2, up);
            std::string us;
            ProtoWriter usw(us);
            usw.real(1, s.cpu_system);
            usw.real(2, s.cpu_system);
            cw.message(3, us);
            if (!s.cpu_model.empty()) {
                cw.string(4, s.cpu_model);
            }
            sw.message(1, c);
        }
        {  // memory (2): physical (1), swap (2) MemoryPool { used, total }
            std::string mem;
            ProtoWriter mw(mem);
            std::string phys;
            ProtoWriter pp(phys);
            pp.int64(1, s.mem_used);
            pp.int64(2, s.mem_total);
            mw.message(1, phys);
            {  // swap (always emitted; the viewer renders a swap widget unconditionally)
                std::string sw2;
                ProtoWriter sp(sw2);
                sp.int64(1, s.swap_used);
                sp.int64(2, s.swap_total);
                mw.message(2, sw2);
            }
            sw.message(2, mem);
        }
        {  // disk (4)
            std::string d;
            ProtoWriter dw(d);
            dw.int64(1, s.disk_used);
            dw.int64(2, s.disk_total);
            sw.message(4, d);
        }
        {  // os (5)
            std::string o;
            ProtoWriter ow(o);
            ow.string(1, s.os_arch);
            ow.string(2, s.os_name);
            ow.string(3, s.os_version);
            sw.message(5, o);
        }
        // java (6): the viewer's Platform view renders "using Java <version>..."
        // unconditionally with a non-null assertion. BDS is native (no JVM), so emit an
        // empty-but-present message to avoid the viewer dereferencing undefined.
        sw.message(6, std::string());
        sw.int64(7, s.uptime_ms);
        w.message(9, ss);
    }

    // sources (13): map<string, PluginOrModMetadata> — the loaded plugins
    for (const PluginInfo &pl : m.plugins) {
        std::string pm;
        ProtoWriter pmw(pm);
        pmw.string(1, pl.name);
        if (!pl.version.empty()) {
            pmw.string(2, pl.version);
        }
        if (!pl.author.empty()) {
            pmw.string(3, pl.author);
        }
        if (!pl.description.empty()) {
            pmw.string(4, pl.description);
        }
        std::string entry;
        ProtoWriter ew(entry);
        ew.string(1, pl.name);
        ew.message(2, pm);
        w.message(13, entry);
    }
    return out;
}

}  // namespace

std::vector<FrameKey> collectFrameKeys(const CallTree &tree)
{
    std::vector<FrameKey> keys;
    std::unordered_set<FrameKey, FrameKeyHash> seen;
    std::vector<const CallTree::Node *> stack;
    for (const auto &[key, child] : tree.root().children) {
        stack.push_back(child.get());
    }
    while (!stack.empty()) {
        const CallTree::Node *node = stack.back();
        stack.pop_back();
        if (seen.insert(node->key).second) {
            keys.push_back(node->key);
        }
        for (const auto &[key, child] : node->children) {
            stack.push_back(child.get());
        }
    }
    return keys;
}

std::string buildSamplerData(const ProfileMetadata &meta, const CallTree &tree,
                             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved)
{
    // The viewer requires a WindowStatistics entry for every time window, so the window
    // set is the union of windows that have samples and windows that have tick stats.
    std::set<std::int32_t> window_set;
    for (const auto &[window, count] : tree.root().times) {
        window_set.insert(window);
    }
    for (const auto &[window, ws] : meta.window_stats) {
        window_set.insert(window);
    }
    if (window_set.empty()) {
        window_set.insert(0);
    }
    std::vector<std::int32_t> windows(window_set.begin(), window_set.end());

    std::vector<std::string> flat;
    std::vector<std::int32_t> top_refs;
    for (const CallTree::Node *kid : sortedChildren(tree.root())) {
        top_refs.push_back(emitNode(kid, windows, meta.interval_us, resolved, flat));
    }

    std::string thread;
    ProtoWriter tw(thread);
    tw.string(1, meta.thread_name);
    for (const std::string &node_bytes : flat) {
        tw.message(3, node_bytes);
    }
    tw.packedDouble(4, alignTimes(tree.root().times, windows, meta.interval_us));
    tw.packedInt32(5, top_refs);

    std::string out;
    ProtoWriter w(out);
    w.message(1, buildMetadata(meta));
    w.message(2, thread);
    w.packedInt32(6, windows);

    // time_window_statistics (7): map<int32, WindowStatistics> — one per time window.
    for (std::int32_t window : windows) {
        auto it = meta.window_stats.find(window);
        WindowStats ws = it != meta.window_stats.end() ? it->second : WindowStats{};
        std::string wstat;
        ProtoWriter ww(wstat);
        ww.int32(1, ws.ticks);
        ww.real(4, ws.tps);
        ww.real(5, ws.mspt_median);
        ww.real(6, ws.mspt_max);
        ww.int32(13, 1000);  // duration ms (per-second window)
        std::string entry;
        ProtoWriter ew(entry);
        ew.int32(1, window);
        ew.message(2, wstat);
        w.message(7, entry);
    }

    return out;
}

}  // namespace spark
