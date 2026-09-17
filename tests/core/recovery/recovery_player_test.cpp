#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

#include "core/recovery/journal_reader.h"
#include "core/recovery/recovery_player.h"
#include "core/recovery/recovery_writer.h"
#include "journal_test_cases.h"
#include "journal_test_support.h"
#include "native/sampler/types.h"
#include "proto/proto_reader.h"

using namespace spark;                // NOLINT(google-build-using-namespace)
using namespace spark::journal_test;  // NOLINT(google-build-using-namespace)

void testRecoveryGrouping()
{
    for (const bool allocation : {false, true}) {
        for (const auto grouping : {ThreadGrouperMode::ByName, ThreadGrouperMode::ByPool, ThreadGrouperMode::AsOne}) {
            for (const bool reverse : {false, true}) {
                const auto dir = makeTempDir();
                std::vector<RecordSpec> records{{.type = RecordType::SessionConfig,
                                                 .sequence = 0,
                                                 .payload = buildSessionConfigPayload(
                                                     4000, 0, true, false, false, static_cast<std::uint8_t>(grouping),
                                                     allocation ? 1 : 0, false, "Console", false, {}, {}, 0)},
                                                {.type = RecordType::ModuleDef,
                                                 .sequence = 1,
                                                 .payload = buildModuleDefPayload(0, "grouping-fixture")}};
                for (const std::uint64_t tid :
                     (reverse ? std::vector<std::uint64_t>{20, 10} : std::vector<std::uint64_t>{10, 20})) {
                    std::string name = "Worker";
                    if (grouping == ThreadGrouperMode::ByPool) {
                        if (allocation) {
                            name = tid == 10 ? "Worker-1 (#254551, session #10)" : "Worker-2 (#254551, session #20)";
                        }
                        else {
                            name = tid == 10 ? "Worker-1 (#10)" : "Worker-2 (#20)";
                        }
                    }
                    records.push_back({.type = RecordType::ThreadDef,
                                       .sequence = static_cast<std::uint32_t>(records.size()),
                                       .payload = buildThreadDefPayload(tid, tid, name)});
                    for (const std::int32_t window : {0, 1}) {
                        Sample sample;
                        sample.thread_id = tid;
                        sample.window = window;
                        sample.weight = window == 0 ? 2000 : 3000;
                        if (tid == 20) {
                            sample.weight = window == 0 ? 5000 : 7000;
                        }
                        sample.frames.push_back({.module = 0, .rva = tid == 10 ? 0x1110ULL : 0x2220ULL});
                        records.push_back({.type = RecordType::Sample,
                                           .sequence = static_cast<std::uint32_t>(records.size()),
                                           .payload = buildSamplePayload(sample)});
                    }
                }
                writeSegmentMulti(dir / "segment-0.jnl", 1000, 0, records);
                const auto result = RecoveryPlayer::replay(dir);
                assert(result.valid);
                assert(verifyGroupedProfile(result.serialized_proto, grouping, allocation));
            }
        }
    }
    std::cout << "testRecoveryGrouping: PASS\n";
}

void testRecoveryRepeatedStackMultipleThreads()
{
    for (const auto grouping : {ThreadGrouperMode::ByName, ThreadGrouperMode::AsOne}) {
        const auto dir = makeTempDir();
        const auto config = buildSessionConfigPayload(4000, 0, true, false, false, static_cast<std::uint8_t>(grouping),
                                                      0, false, "Console", false, {}, {}, 0);
        std::vector<RecordSpec> records{
            {.type = RecordType::SessionConfig, .sequence = 0, .payload = config},
            {.type = RecordType::ModuleDef,
             .sequence = 1,
             .payload = buildModuleDefPayload(0, "repeated-stack-fixture")},
            {.type = RecordType::ThreadDef, .sequence = 2, .payload = buildThreadDefPayload(101, 101, "Worker-1")},
            {.type = RecordType::ThreadDef, .sequence = 3, .payload = buildThreadDefPayload(202, 202, "Worker-2")}};
        for (const std::uint64_t thread_id : {101ULL, 202ULL}) {
            for (const std::int32_t window : {0, 1}) {
                Sample sample;
                sample.thread_id = thread_id;
                sample.tick_id = (thread_id == 101 ? 0 : 2) + static_cast<std::uint64_t>(window);
                sample.window = window;
                sample.weight = window == 0 ? 2000 : 3000;
                sample.frames = {{.module = 0, .rva = 0x1000, .raw_address = 0},
                                 {.module = 0, .rva = 0x1100, .raw_address = 0}};
                records.push_back({.type = RecordType::Sample,
                                   .sequence = static_cast<std::uint32_t>(records.size()),
                                   .payload = buildSamplePayload(sample)});
            }
        }
        writeSegmentMulti(dir / "segment-0.jnl", 1100, 0, records);

        const auto result = RecoveryPlayer::replay(dir);
        assert(result.valid);
        assert(result.sample_count == 4);
        assert(result.thread_count == 2);

        struct ThreadSummary {
            std::string name;
            std::vector<double> weights;
        };
        std::vector<ThreadSummary> summaries;
        std::vector<std::int32_t> windows;
        ProtoReader reader(result.serialized_proto);
        int field = 0;
        int wire_type = 0;
        while (reader.nextField(field, wire_type)) {
            if (field == 2 && wire_type == 2) {
                ProtoReader thread(reader.readMessage());
                ThreadSummary summary;
                while (thread.nextField(field, wire_type)) {
                    if (field == 1 && wire_type == 2) {
                        summary.name = std::string(thread.readString());
                    }
                    else if (field == 4 && wire_type == 2) {
                        summary.weights = groupingWeights(thread.readString());
                    }
                    else {
                        thread.skip(wire_type);
                    }
                }
                assert(thread.valid());
                summaries.push_back(std::move(summary));
            }
            else if (field == 6 && wire_type == 2) {
                ProtoReader packed(reader.readMessage());
                while (!packed.eof()) {
                    windows.push_back(packed.readInt32());
                }
                assert(packed.valid());
            }
            else {
                reader.skip(wire_type);
            }
        }
        assert(reader.valid());
        assert((windows == std::vector<std::int32_t>{0, 1}));
        if (grouping == ThreadGrouperMode::ByName) {
            assert(summaries.size() == 2);
            const std::vector<double> expected_weights{2, 3};
            bool found_worker_one = false;
            bool found_worker_two = false;
            for (const auto &summary : summaries) {
                if (summary.name == "Worker-1") {
                    found_worker_one = true;
                    assert(summary.weights == expected_weights);
                }
                else if (summary.name == "Worker-2") {
                    found_worker_two = true;
                    assert(summary.weights == expected_weights);
                }
            }
            assert(found_worker_one && found_worker_two);
        }
        else {
            assert(summaries.size() == 1);
            assert(summaries[0].name == "All (x2)");
            const std::vector<double> expected_weights{4, 6};
            assert(summaries[0].weights == expected_weights);
        }
    }
    std::cout << "testRecoveryRepeatedStackMultipleThreads: PASS\n";
}

void testLegacyV2Replay()
{
    auto dir = makeTempDir() / "legacy-v2";
    std::filesystem::create_directories(dir);
    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 2;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writeSegmentMulti(
        dir / "segment-0.jnl", 1'000'000, 0,
        {{.type = RecordType::SessionConfig, .sequence = 0, .payload = buildLegacySessionConfigPayload()},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(0, "bedrock_server")},
         {.type = RecordType::Sample, .sequence = 2, .payload = buildSamplePayload(sample)},
         {.type = RecordType::TickEvent, .sequence = 3, .payload = buildTickEventPayload(0, 5.0)}},
        kLegacyJournalVersion);

    const auto journal = JournalReader::readSession(dir);
    assert(journal.valid);
    assert(journal.version == kLegacyJournalVersion);
    assert(journal.session_config.present);
    assert(!journal.session_config.has_window_adjustment);
    const auto result = RecoveryPlayer::replay(dir);
    assert(result.valid);
    assert(result.session_start_ms == 1'120'000);
    std::cout << "testLegacyV2Replay: PASS\n";
}

void testV3GlobalWindowsAndClippedStats()
{
    auto dir = makeTempDir() / "v3-global-windows";
    std::filesystem::create_directories(dir);
    const std::int32_t adjustment = 1000;
    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 20;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    auto config =
        buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, adjustment);
    writeSegmentMulti(
        dir / "segment-0.jnl", 1'210'000, 0,
        {{.type = RecordType::SessionConfig, .sequence = 0, .payload = std::move(config)},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(0, "bedrock_server")},
         {.type = RecordType::Sample, .sequence = 2, .payload = buildSamplePayload(sample)},
         {.type = RecordType::TickEvent, .sequence = 3, .payload = buildTickEventPayload(0, 5.0)}});

    const auto result = RecoveryPlayer::replay(dir);
    assert(result.valid);
    assert(result.session_start_ms == 1'210'000);

    bool found_global_window = false;
    bool found_clipped_stats = false;
    ProtoReader reader(result.serialized_proto);
    int field = 0;
    int wire_type = 0;
    while (reader.nextField(field, wire_type)) {
        if (field == 6 && wire_type == 2) {
            ProtoReader windows(reader.readMessage());
            while (!windows.eof()) {
                if (windows.readVarint() == 20) {
                    found_global_window = true;
                }
            }
        }
        else if (field == 7 && wire_type == 2) {
            ProtoReader entry(reader.readMessage());
            std::int32_t window = 0;
            std::int64_t start = 0;
            std::int64_t end = 0;
            std::int32_t duration = 0;
            while (entry.nextField(field, wire_type)) {
                if (field == 1 && wire_type == 0) {
                    window = entry.readInt32();
                }
                else if (field == 2 && wire_type == 2) {
                    ProtoReader stats(entry.readMessage());
                    while (stats.nextField(field, wire_type)) {
                        if (field == 11 && wire_type == 0) {
                            start = stats.readInt64();
                        }
                        else if (field == 12 && wire_type == 0) {
                            end = stats.readInt64();
                        }
                        else if (field == 13 && wire_type == 0) {
                            duration = stats.readInt32();
                        }
                        else {
                            stats.skip(wire_type);
                        }
                    }
                }
                else {
                    entry.skip(wire_type);
                }
            }
            if (window == 20 && start == 1'210'000 && end == 1'259'000 && duration == 49'000) {
                found_clipped_stats = true;
            }
        }
        else {
            reader.skip(wire_type);
        }
    }
    assert(found_global_window);
    assert(found_clipped_stats);
    std::cout << "testV3GlobalWindowsAndClippedStats: PASS\n";
}

void testRecoveryPlayerReplay()
{
    auto dir = makeTempDir();
    RecoveryWriter::Config cfg;
    cfg.directory = dir / "session-replay";
    cfg.session_id = 100000;
    cfg.flush_interval_ms = 50;
    cfg.sync_interval_ms = 50;

    RecoveryWriter writer(cfg);
    if (!writer.start()) {
        std::cerr << "testRecoveryPlayerReplay: FAIL - writer.start() returned false\n";
        std::exit(1);
    }

    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, "replay test",
                                {"Server thread"}, 0);
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(100, 200, "Server thread");

    Sample sample;
    sample.thread_id = 100;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    sample.frames.push_back({.module = 0, .rva = 0x2000, .raw_address = 0});
    writer.journalSample(sample);

    sample.tick_id = 1;
    sample.window = 1;
    writer.journalSample(sample);

    writer.journalTickEvent(0, 5.0);
    writer.journalTickEvent(1, 10.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.stop();

    auto result = RecoveryPlayer::replay(cfg.directory);
    if (!result.valid) {
        std::cerr << "testRecoveryPlayerReplay: FAIL - " << result.error << "\n";
        std::exit(1);
    }
    assert(result.session_start_ms == 100000);
    assert(result.sample_count == 2);
    assert(result.thread_count == 1);
    assert(result.tick_count == 2);
    assert(!result.has_clean_end);
    assert(!result.serialized_proto.empty());
    std::cout << "testRecoveryPlayerReplay: PASS (proto size=" << result.serialized_proto.size() << ")\n";
}

void testRecoveryPlayerEmptyJournal()
{
    auto dir = makeTempDir();
    auto empty_dir = dir / "empty-session";
    std::filesystem::create_directories(empty_dir);

    auto result = RecoveryPlayer::replay(empty_dir);
    assert(!result.valid);
    std::cout << "testRecoveryPlayerEmptyJournal: PASS\n";
}

void testCleanEndDetected()
{
    auto dir = makeTempDir();
    RecoveryWriter::Config cfg;
    cfg.directory = dir / "clean-session";
    cfg.session_id = 200000;
    cfg.flush_interval_ms = 50;
    cfg.sync_interval_ms = 50;

    RecoveryWriter writer(cfg);
    assert(writer.start());

    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, "clean stop", {}, 0);
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 100, "Server thread");

    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writer.journalSample(sample);
    writer.journalTickEvent(0, 5.0);
    writer.journalCleanEnd();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.stop();

    auto result = RecoveryPlayer::replay(cfg.directory);
    assert(result.valid);
    assert(result.has_clean_end);
    assert(result.serialized_proto.empty());
    std::cout << "testCleanEndDetected: PASS\n";
}

void testNoCleanEndRecovered()
{
    auto dir = makeTempDir();
    RecoveryWriter::Config cfg;
    cfg.directory = dir / "crash-session";
    cfg.session_id = 300000;
    cfg.flush_interval_ms = 50;
    cfg.sync_interval_ms = 50;

    RecoveryWriter writer(cfg);
    assert(writer.start());

    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, "crashed", {}, 0);
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 100, "Server thread");

    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writer.journalSample(sample);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.stop();

    auto result = RecoveryPlayer::replay(cfg.directory);
    assert(result.valid);
    assert(!result.has_clean_end);
    assert(result.sample_count == 1);
    std::cout << "testNoCleanEndRecovered: PASS\n";
}

void testLiveOnlyRefused()
{
    auto dir = makeTempDir();
    RecoveryWriter::Config cfg;
    cfg.directory = dir / "live-only-session";
    cfg.session_id = 400000;
    cfg.flush_interval_ms = 50;
    cfg.sync_interval_ms = 50;

    RecoveryWriter writer(cfg);
    assert(writer.start());

    writer.journalSessionConfig(524287, 0, true, false, false, 1, 1, true, "Console", false, "live-only", {}, 0);
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 100, "Server thread");

    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 524287;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writer.journalSample(sample);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.stop();

    auto result = RecoveryPlayer::replay(cfg.directory);
    assert(!result.valid);
    assert(result.error.find("live-only") != std::string::npos);
    std::cout << "testLiveOnlyRefused: PASS\n";
}

void testCleanEndEarlyExit()
{
    auto dir = makeTempDir() / "clean-early-exit";
    std::filesystem::create_directories(dir);

    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 99, .rva = 0x1000, .raw_address = 0});

    writeSegmentMulti(
        dir / "segment-0.jnl", 500000, 0,
        {{.type = RecordType::SessionConfig,
          .sequence = 0,
          .payload = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0)},
         {.type = RecordType::Sample, .sequence = 1, .payload = buildSamplePayload(sample)},
         {.type = RecordType::CleanEnd, .sequence = 2, .payload = buildCleanEndPayload(1)}});

    auto result = RecoveryPlayer::replay(dir);
    assert(result.valid);
    assert(result.has_clean_end);
    assert(result.serialized_proto.empty());
    std::cout << "testCleanEndEarlyExit: PASS\n";
}

void testNonContiguousModuleId()
{
    auto dir = makeTempDir() / "noncontiguous-modid";
    std::filesystem::create_directories(dir);

    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 20, .rva = 0x1000, .raw_address = 0});

    writeSegmentMulti(
        dir / "segment-0.jnl", 700000, 0,
        {{.type = RecordType::SessionConfig,
          .sequence = 0,
          .payload = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0)},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(20, "bedrock_server")},
         {.type = RecordType::ThreadDef, .sequence = 2, .payload = buildThreadDefPayload(1, 100, "Server thread")},
         {.type = RecordType::Sample, .sequence = 3, .payload = buildSamplePayload(sample)},
         {.type = RecordType::TickEvent, .sequence = 4, .payload = buildTickEventPayload(0, 5.0)}});

    auto result = RecoveryPlayer::replay(dir);
    assert(result.valid);
    assert(!result.serialized_proto.empty());
    assert(result.sample_count == 1);
    std::cout << "testNonContiguousModuleId: PASS (proto size=" << result.serialized_proto.size() << ")\n";
}

void testMissingModuleDefReferenced()
{
    auto dir = makeTempDir() / "missing-moddef";
    std::filesystem::create_directories(dir);

    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 7, .rva = 0x1000, .raw_address = 0});

    writeSegmentMulti(
        dir / "segment-0.jnl", 800000, 0,
        {{.type = RecordType::SessionConfig,
          .sequence = 0,
          .payload = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0)},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(0, "bedrock_server")},
         {.type = RecordType::ThreadDef, .sequence = 2, .payload = buildThreadDefPayload(1, 100, "Server thread")},
         {.type = RecordType::Sample, .sequence = 3, .payload = buildSamplePayload(sample)}});

    auto result = RecoveryPlayer::replay(dir);
    assert(!result.valid);
    assert(result.error.find("missing module id 7") != std::string::npos);
    std::cout << "testMissingModuleDefReferenced: PASS\n";
}
