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

std::vector<double> alignValues(const std::map<std::int32_t, std::uint64_t> &times,
                                const std::vector<std::int32_t> &windows, const ProfileMetadata &meta)
{
    std::vector<double> out(windows.size(), 0.0);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        auto it = times.find(windows[i]);
        if (it == times.end()) {
            continue;
        }
        if (meta.mode == ProfileMode::Allocation) {
            out[i] = static_cast<double>(it->second);
        }
        else {
            out[i] = static_cast<double>(it->second) / 1000.0;
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
    std::ranges::sort(kids, [](const CallTree::Node *a, const CallTree::Node *b) {
        std::uint64_t ta = nodeTotal(*a);
        std::uint64_t tb = nodeTotal(*b);
        return ta != tb ? ta > tb : a->key.rva < b->key.rva;
    });
    return kids;
}

// Post-order flatten: append children first, then this node; return this node's index.
// NOLINTNEXTLINE(misc-no-recursion)
int emitNode(const CallTree::Node *node, const std::vector<std::int32_t> &windows, const ProfileMetadata &meta,
             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved, std::vector<std::string> &flat)
{
    std::vector<std::int32_t> child_refs;
    for (const CallTree::Node *kid : sortedChildren(*node)) {
        child_refs.push_back(emitNode(kid, windows, meta, resolved, flat));
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
        // Private extension fields: preserve sampled PC and unwind root for offline evaluation.
        w.varint(1001, node->key.rva);
        if (it->second.guessed_function_rva != 0) {
            w.varint(1002, it->second.guessed_function_rva);
        }
    }
    w.packedDouble(8, alignValues(node->times, windows, meta));
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
    w.int32(3, m.interval);
    // thread_dumper (4): { type = ALL/SPECIFIC/REGEX, ids, patterns }
    {
        std::string t;
        ProtoWriter tw(t);
        int dumper_type = 1;
        if (m.all_threads) {
            dumper_type = 0;
        }
        else if (m.regex_threads) {
            dumper_type = 2;
        }
        tw.varint(1, dumper_type);
        for (std::int64_t id : m.thread_ids) {
            tw.int64(2, id);
        }
        for (const std::string &pattern : m.thread_patterns) {
            tw.string(3, pattern);
        }
        w.message(4, t);
    }
    // data_aggregator (5): { type, thread_grouper, tick_length_threshold }
    {
        std::string d;
        ProtoWriter dw(d);
        dw.varint(1, m.ticked ? 1 : 0);
        dw.varint(2, static_cast<std::int32_t>(m.thread_grouper));
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
    w.varint(15, m.mode == ProfileMode::Allocation ? 1 : 0);
    w.varint(16, 1);  // sampler_engine = ASYNC
    if (!m.engine_version.empty()) {
        w.string(17, m.engine_version);
    }

    // platform_statistics (8)
    if (m.platform_stats.present) {
        const PlatformStats &p = m.platform_stats;
        std::string ps;
        ProtoWriter pw(ps);
        if (p.process_mem_present || p.process_virtual_present) {  // memory (1) -> heap (1)
            std::string heap;
            ProtoWriter hw(heap);
            if (p.process_mem_present) {
                hw.int64(1, p.process_mem_bytes);
            }
            if (p.process_virtual_present) {
                hw.int64(2, p.process_virtual_bytes);
            }
            std::string mem;
            ProtoWriter mw(mem);
            mw.message(1, heap);
            pw.message(1, mem);
        }
        pw.int64(3, p.uptime_ms);
        if (m.statistics.tps.last_1m.present || m.statistics.tps.last_5m.present ||
            m.statistics.tps.last_15m.present) {  // tps (4)
            std::string t;
            ProtoWriter tw(t);
            if (m.statistics.tps.last_1m.present) {
                tw.real(1, m.statistics.tps.last_1m.value);
            }
            if (m.statistics.tps.last_5m.present) {
                tw.real(2, m.statistics.tps.last_5m.value);
            }
            if (m.statistics.tps.last_15m.present) {
                tw.real(3, m.statistics.tps.last_15m.value);
            }
            tw.int32(4, p.target_tps);
            pw.message(4, t);
        }
        if (m.statistics.mspt.last_1m.present || m.statistics.mspt.last_5m.present) {
            std::string t;
            ProtoWriter tw(t);
            auto write_distribution = [&](int field, const DistributionValues &distribution) {
                if (!distribution.present) {
                    return;
                }
                std::string values;
                ProtoWriter values_writer(values);
                values_writer.real(1, distribution.mean);
                values_writer.real(2, distribution.max);
                values_writer.real(3, distribution.min);
                values_writer.real(4, distribution.median);
                values_writer.real(5, distribution.percentile95);
                tw.message(field, values);
            };
            write_distribution(1, m.statistics.mspt.last_1m);
            write_distribution(2, m.statistics.mspt.last_5m);
            tw.int32(3, p.max_ideal_mspt);
            pw.message(5, t);
        }
        if (p.ping_present) {  // ping (6): Ping { last15m = RollingAverageValues }
            std::string ping;
            ProtoWriter pingw(ping);
            std::string ra;
            ProtoWriter raw(ra);
            raw.real(1, p.ping_mean);
            raw.real(2, p.ping_max);
            raw.real(3, p.ping_min);
            raw.real(4, p.ping_median);
            raw.real(5, p.ping_p95);
            pingw.message(1, ra);
            pw.message(6, ping);
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
            for (const WorldEntry &we : m.world.worlds) {
                std::string world;
                ProtoWriter worldw(world);
                worldw.string(1, we.name);
                worldw.int32(2, we.total_entities);
                for (const WorldRegion &re : we.regions) {
                    std::string region;
                    ProtoWriter regionw(region);
                    regionw.int32(1, re.total_entities);
                    for (const WorldChunk &ce : re.chunks) {
                        std::string chunk;
                        ProtoWriter chunkw(chunk);
                        chunkw.int32(1, ce.x);
                        chunkw.int32(2, ce.z);
                        chunkw.int32(3, ce.total_entities);
                        for (const auto &[type, count] : ce.entity_counts) {
                            std::string entity_count;
                            ProtoWriter entity_count_writer(entity_count);
                            entity_count_writer.string(1, type);
                            entity_count_writer.int32(2, count);
                            chunkw.message(4, entity_count);
                        }
                        regionw.message(2, chunk);
                    }
                    worldw.message(3, region);
                }
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
        if (s.cpu_present || m.statistics.cpu.process_last_1m.present || m.statistics.cpu.process_last_15m.present ||
            m.statistics.cpu.system_last_1m.present || m.statistics.cpu.system_last_15m.present) {  // cpu (1)
            std::string c;
            ProtoWriter cw(c);
            if (s.cpu_present && s.cpu_threads > 0) {
                cw.int32(1, s.cpu_threads);
            }
            if (m.statistics.cpu.process_last_1m.present || m.statistics.cpu.process_last_15m.present) {
                std::string usage;
                ProtoWriter usage_writer(usage);
                if (m.statistics.cpu.process_last_1m.present) {
                    usage_writer.real(1, m.statistics.cpu.process_last_1m.value);
                }
                if (m.statistics.cpu.process_last_15m.present) {
                    usage_writer.real(2, m.statistics.cpu.process_last_15m.value);
                }
                cw.message(2, usage);
            }
            if (m.statistics.cpu.system_last_1m.present || m.statistics.cpu.system_last_15m.present) {
                std::string usage;
                ProtoWriter usage_writer(usage);
                if (m.statistics.cpu.system_last_1m.present) {
                    usage_writer.real(1, m.statistics.cpu.system_last_1m.value);
                }
                if (m.statistics.cpu.system_last_15m.present) {
                    usage_writer.real(2, m.statistics.cpu.system_last_15m.value);
                }
                cw.message(3, usage);
            }
            if (!s.cpu_model.empty()) {
                cw.string(4, s.cpu_model);
            }
            sw.message(1, c);
        }
        if (s.memory_present || s.swap_present) {  // memory (2): physical (1), swap (2)
            std::string mem;
            ProtoWriter mw(mem);
            if (s.memory_present) {
                std::string phys;
                ProtoWriter pp(phys);
                pp.int64(1, s.mem_used);
                pp.int64(2, s.mem_total);
                mw.message(1, phys);
            }
            if (s.swap_present) {
                std::string sw2;
                ProtoWriter sp(sw2);
                sp.int64(1, s.swap_used);
                sp.int64(2, s.swap_total);
                mw.message(2, sw2);
            }
            sw.message(2, mem);
        }
        if (s.disk_present) {  // disk (4)
            std::string d;
            ProtoWriter dw(d);
            dw.int64(1, s.disk_used);
            dw.int64(2, s.disk_total);
            sw.message(4, d);
        }
        if (s.os_present) {  // os (5)
            std::string o;
            ProtoWriter ow(o);
            if (!s.os_arch.empty()) {
                ow.string(1, s.os_arch);
            }
            if (!s.os_name.empty()) {
                ow.string(2, s.os_name);
            }
            if (!s.os_version.empty()) {
                ow.string(3, s.os_version);
            }
            sw.message(5, o);
        }
        // java (6): the viewer's Platform view renders "using Java <version>..."
        // unconditionally with a non-null assertion. BDS is native (no JVM), so emit an
        // empty-but-present message to avoid the viewer dereferencing undefined.
        sw.message(6, std::string());
        if (s.uptime_present) {
            sw.int64(7, s.uptime_ms);
        }
        if (s.net_present) {  // net (8): map<string, NetInterface>
            for (const auto &[name, snap] : s.net_averages) {
                std::string netif;
                ProtoWriter nw(netif);
                auto write_rate = [&nw](int field, const NetworkRateValues &rate) {
                    if (!rate.present) {
                        return;
                    }
                    std::string ra;
                    ProtoWriter raw(ra);
                    raw.real(1, rate.mean);
                    raw.real(2, rate.max);
                    raw.real(3, rate.min);
                    raw.real(4, rate.median);
                    raw.real(5, rate.percentile95);
                    nw.message(field, ra);
                };
                write_rate(1, snap.rx_bytes_per_second);
                write_rate(2, snap.tx_bytes_per_second);
                write_rate(3, snap.rx_packets_per_second);
                write_rate(4, snap.tx_packets_per_second);
                std::string entry;
                ProtoWriter ew(entry);
                ew.string(1, name);
                ew.message(2, netif);
                sw.message(8, entry);
            }
        }
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

    // extra_platform_metadata (14): map<string, string>. This is the only
    // extensible metadata surface in the upstream spark schema, so native-only
    // diagnostics such as captured/dropped allocation samples are placed here.
    for (const auto &[key, value] : m.extra_platform_metadata) {
        std::string entry;
        ProtoWriter ew(entry);
        ew.string(1, key);
        ew.string(2, value);
        w.message(14, entry);
    }
    // server_configurations (10): map<string, string> - allowlisted
    // server.properties entries for performance diagnostics.
    for (const auto &[key, value] : m.server_configurations) {
        std::string entry;
        ProtoWriter ew(entry);
        ew.string(1, key);
        ew.string(2, value);
        w.message(10, entry);
    }
    return out;
}

}  // namespace

std::vector<FrameKey> collectFrameKeys(const CallTree &tree)
{
    return collectFrameKeys({ThreadTreeView{.name = "", .tree = &tree}});
}

std::vector<FrameKey> collectFrameKeys(const std::vector<ThreadTreeView> &threads)
{
    std::vector<FrameKey> keys;
    std::unordered_set<FrameKey, FrameKeyHash> seen;
    std::vector<const CallTree::Node *> stack;
    for (const ThreadTreeView &thread : threads) {
        if (thread.tree == nullptr) {
            continue;
        }
        for (const auto &[key, child] : thread.tree->root().children) {
            stack.push_back(child.get());
        }
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
    return buildSamplerData(meta, {ThreadTreeView{.name = meta.thread_name, .tree = &tree}}, resolved);
}

std::string buildSamplerData(const ProfileMetadata &meta, const std::vector<ThreadTreeView> &threads,
                             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved)
{
    // The viewer requires a WindowStatistics entry for every time window, so the window
    // set is the union of windows that have samples and windows that have tick stats.
    std::set<std::int32_t> window_set;
    for (const ThreadTreeView &thread : threads) {
        if (thread.tree == nullptr) {
            continue;
        }
        for (const auto &[window, count] : thread.tree->root().times) {
            window_set.insert(window);
        }
    }
    for (const auto &[window, ws] : meta.window_stats) {
        window_set.insert(window);
    }
    if (window_set.empty()) {
        window_set.insert(0);
    }
    std::vector<std::int32_t> windows(window_set.begin(), window_set.end());

    std::string out;
    ProtoWriter w(out);
    w.message(1, buildMetadata(meta));
    for (const ThreadTreeView &thread_view : threads) {
        if (thread_view.tree == nullptr) {
            continue;
        }
        std::vector<std::string> flat;
        std::vector<std::int32_t> top_refs;
        for (const CallTree::Node *kid : sortedChildren(thread_view.tree->root())) {
            top_refs.push_back(emitNode(kid, windows, meta, resolved, flat));
        }

        std::string thread;
        ProtoWriter tw(thread);
        tw.string(1, thread_view.name);
        for (const std::string &node_bytes : flat) {
            tw.message(3, node_bytes);
        }
        tw.packedDouble(4, alignValues(thread_view.tree->root().times, windows, meta));
        tw.packedInt32(5, top_refs);
        w.message(2, thread);
    }
    w.packedInt32(6, windows);

    // time_window_statistics (7): map<int32, WindowStatistics> — one per time window.
    for (std::int32_t window : windows) {
        auto it = meta.window_stats.find(window);
        WindowStats ws = it != meta.window_stats.end() ? it->second : WindowStats{};
        std::string wstat;
        ProtoWriter ww(wstat);
        if (ws.ticks_present) {
            ww.int32(1, ws.ticks);
        }
        if (ws.cpu_process_present) {
            ww.real(2, ws.cpu_process);
        }
        if (ws.cpu_system_present) {
            ww.real(3, ws.cpu_system);
        }
        if (ws.tps_present) {
            ww.real(4, ws.tps);
        }
        if (ws.mspt_present) {
            ww.real(5, ws.mspt_median);
            ww.real(6, ws.mspt_max);
        }
        if (ws.players_present) {
            ww.int32(7, ws.players);
        }
        if (ws.entities_present) {
            ww.int32(8, ws.entities);
        }
        if (ws.chunks_present) {
            ww.int32(10, ws.chunks);
        }
        if (ws.duration_ms > 0) {
            ww.int64(11, ws.start_time_ms);
            ww.int64(12, ws.end_time_ms);
            ww.int32(13, ws.duration_ms);
        }
        std::string entry;
        ProtoWriter ew(entry);
        ew.int32(1, window);
        ew.message(2, wstat);
        w.message(7, entry);
    }

    // socket_channel_info (8): SocketChannelInfo for live viewer.
    if (!meta.socket_channel_info_proto.empty()) {
        w.message(8, meta.socket_channel_info_proto);
    }

    return out;
}

std::string buildHealthData(const HealthData &data)
{
    std::string out;
    ProtoWriter w(out);

    // HealthMetadata (1)
    {
        std::string meta;
        ProtoWriter mw(meta);

        // creator (1): CommandSenderMetadata
        {
            std::string c;
            ProtoWriter cw(c);
            cw.varint(1, data.creator_is_player ? 1 : 0);
            cw.string(2, data.creator_name);
            mw.message(1, c);
        }

        // platform_metadata (2): PlatformMetadata
        {
            std::string p;
            ProtoWriter pw(p);
            pw.varint(1, 0);  // type = SERVER
            pw.string(2, "Endstone");
            pw.string(3, data.endstone_version);
            if (!data.minecraft_version.empty()) {
                pw.string(4, data.minecraft_version);
            }
            pw.int32(7, kSparkFormatVersion);
            pw.string(8, "Endstone");
            mw.message(2, p);
        }

        // platform_statistics (3): PlatformStatistics
        if (data.platform_stats.present) {
            const PlatformStats &p = data.platform_stats;
            std::string ps;
            ProtoWriter pw(ps);
            if (p.process_mem_present || p.process_virtual_present) {
                std::string heap;
                ProtoWriter hw(heap);
                if (p.process_mem_present) {
                    hw.int64(1, p.process_mem_bytes);
                }
                if (p.process_virtual_present) {
                    hw.int64(2, p.process_virtual_bytes);
                }
                std::string mem;
                ProtoWriter mw2(mem);
                mw2.message(1, heap);
                pw.message(1, mem);
            }
            pw.int64(3, p.uptime_ms);
            if (data.statistics.tps.last_1m.present || data.statistics.tps.last_5m.present ||
                data.statistics.tps.last_15m.present) {
                std::string t;
                ProtoWriter tw(t);
                if (data.statistics.tps.last_1m.present) {
                    tw.real(1, data.statistics.tps.last_1m.value);
                }
                if (data.statistics.tps.last_5m.present) {
                    tw.real(2, data.statistics.tps.last_5m.value);
                }
                if (data.statistics.tps.last_15m.present) {
                    tw.real(3, data.statistics.tps.last_15m.value);
                }
                tw.int32(4, p.target_tps);
                pw.message(4, t);
            }
            if (data.statistics.mspt.last_1m.present || data.statistics.mspt.last_5m.present) {
                std::string t;
                ProtoWriter tw(t);
                auto write_distribution = [&](int field, const DistributionValues &distribution) {
                    if (!distribution.present) {
                        return;
                    }
                    std::string values;
                    ProtoWriter values_writer(values);
                    values_writer.real(1, distribution.mean);
                    values_writer.real(2, distribution.max);
                    values_writer.real(3, distribution.min);
                    values_writer.real(4, distribution.median);
                    values_writer.real(5, distribution.percentile95);
                    tw.message(field, values);
                };
                write_distribution(1, data.statistics.mspt.last_1m);
                write_distribution(2, data.statistics.mspt.last_5m);
                tw.int32(3, p.max_ideal_mspt);
                pw.message(5, t);
            }
            if (p.ping_present) {
                std::string ping;
                ProtoWriter pingw(ping);
                std::string ra;
                ProtoWriter raw(ra);
                raw.real(1, p.ping_mean);
                raw.real(2, p.ping_max);
                raw.real(3, p.ping_min);
                raw.real(4, p.ping_median);
                raw.real(5, p.ping_p95);
                pingw.message(1, ra);
                pw.message(6, ping);
            }
            if (p.player_count >= 0) {
                pw.int64(7, p.player_count);
            }
            if (p.online_mode > 0) {
                pw.varint(9, static_cast<std::uint64_t>(p.online_mode));
            }
            mw.message(3, ps);
        }

        // system_statistics (4): SystemStatistics
        if (data.system_stats.present) {
            const SystemStats &s = data.system_stats;
            std::string ss;
            ProtoWriter sw(ss);
            if (s.cpu_present || data.statistics.cpu.process_last_1m.present ||
                data.statistics.cpu.process_last_15m.present || data.statistics.cpu.system_last_1m.present ||
                data.statistics.cpu.system_last_15m.present) {
                std::string c;
                ProtoWriter cw(c);
                if (s.cpu_present && s.cpu_threads > 0) {
                    cw.int32(1, s.cpu_threads);
                }
                if (data.statistics.cpu.process_last_1m.present || data.statistics.cpu.process_last_15m.present) {
                    std::string usage;
                    ProtoWriter usage_writer(usage);
                    if (data.statistics.cpu.process_last_1m.present) {
                        usage_writer.real(1, data.statistics.cpu.process_last_1m.value);
                    }
                    if (data.statistics.cpu.process_last_15m.present) {
                        usage_writer.real(2, data.statistics.cpu.process_last_15m.value);
                    }
                    cw.message(2, usage);
                }
                if (data.statistics.cpu.system_last_1m.present || data.statistics.cpu.system_last_15m.present) {
                    std::string usage;
                    ProtoWriter usage_writer(usage);
                    if (data.statistics.cpu.system_last_1m.present) {
                        usage_writer.real(1, data.statistics.cpu.system_last_1m.value);
                    }
                    if (data.statistics.cpu.system_last_15m.present) {
                        usage_writer.real(2, data.statistics.cpu.system_last_15m.value);
                    }
                    cw.message(3, usage);
                }
                if (!s.cpu_model.empty()) {
                    cw.string(4, s.cpu_model);
                }
                sw.message(1, c);
            }
            if (s.memory_present || s.swap_present) {
                std::string mem;
                ProtoWriter mw2(mem);
                if (s.memory_present) {
                    std::string phys;
                    ProtoWriter pp(phys);
                    pp.int64(1, s.mem_used);
                    pp.int64(2, s.mem_total);
                    mw2.message(1, phys);
                }
                if (s.swap_present) {
                    std::string sw2;
                    ProtoWriter sp(sw2);
                    sp.int64(1, s.swap_used);
                    sp.int64(2, s.swap_total);
                    mw2.message(2, sw2);
                }
                sw.message(2, mem);
            }
            if (s.disk_present) {
                std::string d;
                ProtoWriter dw(d);
                dw.int64(1, s.disk_used);
                dw.int64(2, s.disk_total);
                sw.message(4, d);
            }
            if (s.os_present) {
                std::string o;
                ProtoWriter ow(o);
                if (!s.os_arch.empty()) {
                    ow.string(1, s.os_arch);
                }
                if (!s.os_name.empty()) {
                    ow.string(2, s.os_name);
                }
                if (!s.os_version.empty()) {
                    ow.string(3, s.os_version);
                }
                sw.message(5, o);
            }
            sw.message(6, std::string());  // java: empty-but-present (no JVM)
            if (s.uptime_present) {
                sw.int64(7, s.uptime_ms);
            }
            if (s.net_present) {
                for (const auto &[name, snap] : s.net_averages) {
                    std::string netif;
                    ProtoWriter nw(netif);
                    auto write_rate = [&nw](int field, const NetworkRateValues &rate) {
                        if (!rate.present) {
                            return;
                        }
                        std::string ra;
                        ProtoWriter raw(ra);
                        raw.real(1, rate.mean);
                        raw.real(2, rate.max);
                        raw.real(3, rate.min);
                        raw.real(4, rate.median);
                        raw.real(5, rate.percentile95);
                        nw.message(field, ra);
                    };
                    write_rate(1, snap.rx_bytes_per_second);
                    write_rate(2, snap.tx_bytes_per_second);
                    write_rate(3, snap.rx_packets_per_second);
                    write_rate(4, snap.tx_packets_per_second);
                    std::string entry;
                    ProtoWriter ew(entry);
                    ew.string(1, name);
                    ew.message(2, netif);
                    sw.message(8, entry);
                }
            }
            mw.message(4, ss);
        }

        // generated_time (5)
        mw.int64(5, data.generated_time_ms);

        // sources (7): map<string, PluginOrModMetadata>
        for (const PluginInfo &pl : data.plugins) {
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
            mw.message(7, entry);
        }

        // extra_platform_metadata (8): map<string, string>
        for (const auto &[key, value] : data.extra_platform_metadata) {
            std::string entry;
            ProtoWriter ew(entry);
            ew.string(1, key);
            ew.string(2, value);
            mw.message(8, entry);
        }

        w.message(1, meta);
    }

    // time_window_statistics (2): map<int32, WindowStatistics>
    for (const auto &[window, ws] : data.window_stats) {
        std::string wstat;
        ProtoWriter ww(wstat);
        if (ws.ticks_present) {
            ww.int32(1, ws.ticks);
        }
        if (ws.cpu_process_present) {
            ww.real(2, ws.cpu_process);
        }
        if (ws.cpu_system_present) {
            ww.real(3, ws.cpu_system);
        }
        if (ws.tps_present) {
            ww.real(4, ws.tps);
        }
        if (ws.mspt_present) {
            ww.real(5, ws.mspt_median);
            ww.real(6, ws.mspt_max);
        }
        if (ws.players_present) {
            ww.int32(7, ws.players);
        }
        if (ws.entities_present) {
            ww.int32(8, ws.entities);
        }
        if (ws.chunks_present) {
            ww.int32(10, ws.chunks);
        }
        if (ws.duration_ms > 0) {
            ww.int64(11, ws.start_time_ms);
            ww.int64(12, ws.end_time_ms);
            ww.int32(13, ws.duration_ms);
        }
        std::string entry;
        ProtoWriter ew(entry);
        ew.int32(1, window);
        ew.message(2, wstat);
        w.message(2, entry);
    }

    return out;
}

}  // namespace spark
