#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "core/recovery/journal_reader.h"
#include "core/recovery/recovery_player.h"
#include "core/recovery/recovery_writer.h"
#include "journal_test_cases.h"
#include "journal_test_support.h"
#include "native/sampler/types.h"
#include "profiling_window.h"
#include "proto/proto_reader.h"

using namespace spark;                // NOLINT(google-build-using-namespace)
using namespace spark::journal_test;  // NOLINT(google-build-using-namespace)

void testRollingJournalRecovery()
{
    auto dir = makeTempDir() / "rolling";
    std::filesystem::create_directories(dir);

    RecoveryWriter::Config cfg;
    cfg.directory = dir;
    cfg.session_id = 900000;
    cfg.max_segment_bytes = 512;
    cfg.max_total_bytes = 1024;
    cfg.flush_interval_ms = 20;
    cfg.sync_interval_ms = 20;
    cfg.shutdown_timeout_ms = 15000;

    RecoveryWriter writer(cfg);
    if (!writer.start()) {
        std::cerr << "testRollingJournalRecovery: FAIL - writer.start() returned false\n";
        std::exit(1);
    }

    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, "rolling test", {}, 0);
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalModuleDef(1, "libfoo.so");
    writer.journalThreadDef(100, 200, "Server thread");

    for (int i = 0; i < 200; ++i) {
        Sample sample;
        sample.thread_id = 100;
        sample.tick_id = static_cast<std::uint64_t>(i);
        sample.window = i / 50;
        sample.weight = 4000;
        sample.frames.push_back({.module = 0, .rva = static_cast<std::uint64_t>(0x1000 + i), .raw_address = 0});
        sample.frames.push_back({.module = 1, .rva = static_cast<std::uint64_t>(0x2000 + i), .raw_address = 0});
        writer.journalSample(sample);
        writer.journalTickEvent(static_cast<std::uint64_t>(i), 5.0);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    writer.stop();

    assert(!std::filesystem::exists(dir / "segment-0.jnl"));
    assert(std::filesystem::exists(dir / "metadata.snapshot"));

    auto journal = JournalReader::readSession(dir);
    assert(journal.valid);
    assert(!journal.head_truncated);
    if (!journal.metadata_snapshot) {
        std::cerr << "testRollingJournalRecovery: FAIL - metadata snapshot missing\n";
        std::exit(1);
    }
    const MetadataSnapshot &snapshot = *journal.metadata_snapshot;
    assert(snapshot.valid);
    assert(snapshot.session_id == 900000);

    auto result = RecoveryPlayer::replay(dir);
    if (!result.valid) {
        std::cerr << "testRollingJournalRecovery: FAIL - " << result.error << "\n";
        std::exit(1);
    }
    assert(!result.has_clean_end);
    assert(result.sample_count > 0);
    assert(result.thread_count >= 1);
    assert(result.session_start_ms == 900000);
    assert(result.tick_count > 0);
    assert(result.tick_count < 200);
    assert(!result.serialized_proto.empty());
    std::cout << "testRollingJournalRecovery: PASS (samples=" << result.sample_count
              << ", proto=" << result.serialized_proto.size() << ")\n";
}

void testCorruptSnapshotWrongSession()
{
    auto dir = makeTempDir() / "corrupt-snapshot-session";
    std::filesystem::create_directories(dir);

    RecoveryWriter::Config cfg;
    cfg.directory = dir;
    cfg.session_id = 910000;
    cfg.max_segment_bytes = 512;
    cfg.max_total_bytes = 1024;
    cfg.flush_interval_ms = 20;
    cfg.sync_interval_ms = 20;
    cfg.shutdown_timeout_ms = 15000;

    RecoveryWriter writer(cfg);
    assert(writer.start());
    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0);
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 100, "Server thread");
    for (int i = 0; i < 200; ++i) {
        Sample sample;
        sample.thread_id = 1;
        sample.tick_id = static_cast<std::uint64_t>(i);
        sample.window = 0;
        sample.weight = 4000;
        sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
        writer.journalSample(sample);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    writer.stop();

    assert(!std::filesystem::exists(dir / "segment-0.jnl"));
    assert(std::filesystem::exists(dir / "metadata.snapshot"));

    std::vector<SnapshotModuleDef> modules{{.module_id = 0, .path = "bedrock_server"}};
    std::vector<SnapshotThreadDef> threads{{.thread_id = 1, .os_thread_id = 100, .name = "Server thread"}};
    auto buf = serializeMetadataSnapshot(999999, 0, {}, modules, threads);
    writeBytes(dir / "metadata.snapshot", buf);

    auto result = RecoveryPlayer::replay(dir);
    assert(!result.valid);
    assert(result.error.find("head-truncated") != std::string::npos);
    std::cout << "testCorruptSnapshotWrongSession: PASS\n";
}

void testTruncatedSnapshot()
{
    auto dir = makeTempDir() / "truncated-snapshot";
    std::filesystem::create_directories(dir);

    Sample sample;
    sample.thread_id = 1;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writeSegmentMulti(dir / "segment-3.jnl", 920000, 3,
                      {{.type = RecordType::Sample, .sequence = 0, .payload = buildSamplePayload(sample)}});
    const std::vector<std::uint8_t> partial{'S', 'P', 'R', 'K', 'M', 'E', 'T', 'A'};
    writeBytes(dir / "metadata.snapshot", partial);

    auto journal = JournalReader::readSession(dir);
    assert(journal.valid);
    assert(journal.head_truncated);

    auto result = RecoveryPlayer::replay(dir);
    assert(!result.valid);
    std::cout << "testTruncatedSnapshot: PASS\n";
}

void testAllocationSentinelModule0()
{
    auto dir = makeTempDir() / "alloc-mod0";
    std::filesystem::create_directories(dir);

    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 524287;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});

    writeSegmentMulti(
        dir / "segment-0.jnl", 930000, 0,
        {{.type = RecordType::SessionConfig,
          .sequence = 0,
          .payload =
              buildSessionConfigPayload(524287, 0, true, false, false, 1, 1, false, "Console", false, {}, {}, 0)},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(0, kOtherModulesSentinel)},
         {.type = RecordType::ModuleDef, .sequence = 2, .payload = buildModuleDefPayload(1, "bedrock_server")},
         {.type = RecordType::ThreadDef, .sequence = 3, .payload = buildThreadDefPayload(1, 100, "Server thread")},
         {.type = RecordType::Sample, .sequence = 4, .payload = buildSamplePayload(sample)},
         {.type = RecordType::TickEvent, .sequence = 5, .payload = buildTickEventPayload(0, 5.0)}});

    auto result = RecoveryPlayer::replay(dir);
    assert(result.valid);
    assert(result.sample_count == 1);
    assert(!result.serialized_proto.empty());
    std::cout << "testAllocationSentinelModule0: PASS\n";
}

void testMissingSentinelModule0()
{
    auto dir = makeTempDir() / "missing-mod0";
    std::filesystem::create_directories(dir);

    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});

    writeSegmentMulti(
        dir / "segment-0.jnl", 940000, 0,
        {{.type = RecordType::SessionConfig,
          .sequence = 0,
          .payload = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0)},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(1, "bedrock_server")},
         {.type = RecordType::ThreadDef, .sequence = 2, .payload = buildThreadDefPayload(1, 100, "Server thread")},
         {.type = RecordType::Sample, .sequence = 3, .payload = buildSamplePayload(sample)}});

    auto result = RecoveryPlayer::replay(dir);
    assert(!result.valid);
    assert(result.error.find("missing module id 0") != std::string::npos);
    std::cout << "testMissingSentinelModule0: PASS\n";
}

void testStopWithoutCleanEndRecoverable()
{
    auto dir = makeTempDir();
    RecoveryWriter::Config cfg;
    cfg.directory = dir;
    cfg.session_id = 950000;
    cfg.flush_interval_ms = 50;
    cfg.sync_interval_ms = 50;
    cfg.shutdown_timeout_ms = 15000;

    RecoveryWriter writer(cfg);
    assert(writer.start());
    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0);
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
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.stop();

    auto result = RecoveryPlayer::replay(cfg.directory);
    assert(result.valid);
    assert(!result.has_clean_end);
    assert(result.sample_count == 1);
    assert(!result.serialized_proto.empty());
    std::cout << "testStopWithoutCleanEndRecoverable: PASS\n";
}

void testSnapshotOnlyThreadIsNotExported()
{
    auto dir = makeTempDir() / "snapshot-only-thread";
    std::filesystem::create_directories(dir);

    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 100;
    sample.window = 5;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writeSegmentMulti(dir / "segment-3.jnl", 970000, 3,
                      {{.type = RecordType::Sample, .sequence = 10, .payload = buildSamplePayload(sample)},
                       {.type = RecordType::TickEvent, .sequence = 11, .payload = buildTickEventPayload(100, 5.0)}});

    const std::vector<SnapshotModuleDef> modules{{.module_id = 0, .path = "bedrock_server"}};
    const std::vector<SnapshotThreadDef> threads{{.thread_id = 1, .os_thread_id = 100, .name = "Server thread"},
                                                 {.thread_id = 2, .os_thread_id = 200, .name = "Historical worker"}};
    auto session_config =
        buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0);
    writeBytes(dir / "metadata.snapshot",
               serializeMetadataSnapshot(970000, 0, session_config.take(), modules, threads));

    const auto result = RecoveryPlayer::replay(dir);
    assert(result.valid);
    assert(result.thread_count == 1);
    assert(result.tick_count == 1);
    assert(result.session_start_ms == 970000);
    std::cout << "testSnapshotOnlyThreadIsNotExported: PASS\n";
}

void testLegacyV2RebasesFromMinimumRetainedWindow()
{
    auto dir = makeTempDir() / "legacy-history";
    std::filesystem::create_directories(dir);
    Sample discarded;
    discarded.thread_id = 1;
    discarded.tick_id = 1;
    discarded.window = 39;
    discarded.weight = 4000;
    discarded.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    Sample retained = discarded;
    retained.tick_id = 2;
    retained.window = 100;
    writeSegmentMulti(
        dir / "segment-0.jnl", 1'000'000, 0,
        {{.type = RecordType::SessionConfig, .sequence = 0, .payload = buildLegacySessionConfigPayload()},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(0, "bedrock_server")},
         {.type = RecordType::Sample, .sequence = 2, .payload = buildSamplePayload(discarded)},
         {.type = RecordType::Sample, .sequence = 3, .payload = buildSamplePayload(retained)}},
        kLegacyJournalVersion);

    const auto result = RecoveryPlayer::replay(dir);
    assert(result.valid);
    assert(result.sample_count == 1);
    assert(result.tick_count == 1);
    assert(result.session_start_ms == 1'000'000 + 100 * profiling_window::kSizeMs);
    std::cout << "testLegacyV2RebasesFromMinimumRetainedWindow: PASS\n";
}

namespace {

void assertHistoryWindows(const std::string &serialized)
{
    std::vector<std::int32_t> windows;
    ProtoReader reader(serialized);
    int field = 0;
    int wire_type = 0;
    while (reader.nextField(field, wire_type)) {
        if (field == 6 && wire_type == 2) {
            ProtoReader packed(reader.readMessage());
            while (!packed.eof()) {
                windows.push_back(packed.readInt32());
            }
        }
        else {
            reader.skip(wire_type);
        }
    }
    assert(reader.valid());
    assert((windows == std::vector<std::int32_t>{40, 100}));
}

RecoveredProfile replayHistoryCase(bool allocation)
{
    auto dir = makeTempDir() / (allocation ? "history-allocation" : "history-execution");
    std::filesystem::create_directories(dir);

    const auto config = buildSessionConfigPayload(4000, 0, false, false, false, 1, allocation ? 1 : 0, false, "Console",
                                                  false, "history", {}, 0);
    Sample first;
    first.thread_id = 1;
    first.tick_id = 1;
    first.window = 39;
    first.weight = allocation ? 524287 : 4000;
    first.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    Sample second = first;
    second.tick_id = 2;
    second.window = 40;
    Sample third = first;
    third.tick_id = 3;
    third.window = 100;
    const auto module_path = allocation ? kOtherModulesSentinel : std::string_view("bedrock_server");
    writeSegmentMulti(
        dir / "segment-0.jnl", 0, 0,
        {{.type = RecordType::SessionConfig, .sequence = 0, .payload = config},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(0, module_path)},
         {.type = RecordType::ThreadDef, .sequence = 2, .payload = buildThreadDefPayload(1, 100, "Server thread")},
         {.type = RecordType::Sample, .sequence = 3, .payload = buildSamplePayload(first)},
         {.type = RecordType::Sample, .sequence = 4, .payload = buildSamplePayload(second)},
         {.type = RecordType::Sample, .sequence = 5, .payload = buildSamplePayload(third)},
         {.type = RecordType::TickEvent, .sequence = 6, .payload = buildTickEventPayload(0, 4.0)},
         {.type = RecordType::TickEvent, .sequence = 7, .payload = buildTickEventPayload(2, 5.0)},
         {.type = RecordType::TickEvent, .sequence = 8, .payload = buildTickEventPayload(3, 6.0)},
         {.type = RecordType::TickEvent, .sequence = 9, .payload = buildTickEventPayload(4, 7.0)}});
    return RecoveryPlayer::replay(dir);
}

}  // namespace

void testRecoveryHistoryExecution()
{
    const auto result = replayHistoryCase(false);
    assert(result.valid);
    assert(result.sample_count == 2);
    assert(result.tick_count == 2);
    assert(result.session_start_ms == profiling_window::windowStartTime(40, 0));
    assertHistoryWindows(result.serialized_proto);
    std::cout << "testRecoveryHistoryExecution: PASS\n";
}

void testRecoveryHistoryCumulativeAllocation()
{
    const auto result = replayHistoryCase(true);
    assert(result.valid);
    assert(result.sample_count == 2);
    assert(result.tick_count == 2);
    assert(result.session_start_ms == profiling_window::windowStartTime(40, 0));
    assertHistoryWindows(result.serialized_proto);
    std::cout << "testRecoveryHistoryCumulativeAllocation: PASS\n";
}

void testRecoveryRejectsExtremeIntegers()
{
    auto session_dir = makeTempDir() / "session-id-overflow";
    std::filesystem::create_directories(session_dir);
    writeSegment(session_dir / "segment-0.jnl", std::numeric_limits<std::uint64_t>::max(), 0, 0, RecordType::TickEvent,
                 buildTickEventPayload(0, 1.0));
    auto session_result = RecoveryPlayer::replay(session_dir);
    assert(!session_result.valid);
    assert(session_result.error.find("session_id") != std::string::npos);

    auto tick_dir = makeTempDir() / "tick-span-overflow";
    std::filesystem::create_directories(tick_dir);
    Sample low;
    low.thread_id = 1;
    low.tick_id = 0;
    low.window = 0;
    low.weight = 4000;
    low.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    Sample high = low;
    high.tick_id = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    writeSegmentMulti(
        tick_dir / "segment-0.jnl", 1, 0,
        {{.type = RecordType::SessionConfig,
          .sequence = 0,
          .payload = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0)},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(0, "bedrock_server")},
         {.type = RecordType::Sample, .sequence = 2, .payload = buildSamplePayload(low)},
         {.type = RecordType::Sample, .sequence = 3, .payload = buildSamplePayload(high)}});
    auto tick_result = RecoveryPlayer::replay(tick_dir);
    assert(!tick_result.valid);
    assert(tick_result.error.find("tick span") != std::string::npos);

    auto legacy_dir = makeTempDir() / "legacy-session-overflow";
    std::filesystem::create_directories(legacy_dir);
    low.tick_id = 0;
    low.window = 1;
    writeSegmentMulti(
        legacy_dir / "segment-0.jnl", static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), 0,
        {{.type = RecordType::SessionConfig, .sequence = 0, .payload = buildLegacySessionConfigPayload()},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(0, "bedrock_server")},
         {.type = RecordType::Sample, .sequence = 2, .payload = buildSamplePayload(low)}},
        kLegacyJournalVersion);
    auto legacy_result = RecoveryPlayer::replay(legacy_dir);
    assert(!legacy_result.valid);
    assert(legacy_result.error.find("overflows") != std::string::npos);
    std::cout << "testRecoveryRejectsExtremeIntegers: PASS\n";
}
