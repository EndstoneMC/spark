#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <thread>

#include "core/recovery/journal_format.h"
#include "core/recovery/journal_reader.h"
#include "core/recovery/recovery_player.h"
#include "core/recovery/recovery_writer.h"
#include "native/sampler/types.h"

using namespace spark;  // NOLINT(google-build-using-namespace)

namespace {

std::filesystem::path makeTempDir()
{
    auto base = std::filesystem::temp_directory_path() / "spark_journal_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    return base;
}

void testFileHeaderMagic()
{
    auto header = serializeFileHeader(42, 1000, 0);
    assert(header.size() == kFileHeaderSize);
    assert(std::memcmp(header.data(), kJournalMagic, 8) == 0);

    std::uint16_t version;
    std::memcpy(&version, header.data() + 8, 2);
    assert(version == kJournalVersion);
    std::cout << "testFileHeaderMagic: PASS\n";
}

void testRecordSerialization()
{
    JournalBuffer payload = buildTickEventPayload(7, 42.5);
    auto record = serializeRecord(RecordType::TickEvent, 1, payload);

    // Record header: 1 (type) + 1 (reserved) + 4 (seq) + 4 (len) + 4 (crc) = 14
    assert(record.size() == kRecordHeaderSize + payload.size());
    assert(record[0] == static_cast<std::uint8_t>(RecordType::TickEvent));
    std::cout << "testRecordSerialization: PASS\n";
}

void testModuleDefRoundTrip()
{
    JournalBuffer payload = buildModuleDefPayload(5, "/usr/lib/libc.so");
    auto record = serializeRecord(RecordType::ModuleDef, 0, payload);

    // Parse it back.
    JournalReadResult result;
    // Simulate a file: header + record.
    auto header = serializeFileHeader(1, 0, 0);
    std::vector<std::uint8_t> file_buf;
    file_buf.insert(file_buf.end(), header.begin(), header.end());
    file_buf.insert(file_buf.end(), record.begin(), record.end());

    // Write to temp file and read.
    auto dir = makeTempDir();
    auto path = dir / "segment-0.jnl";
    std::FILE *f = std::fopen(path.string().c_str(), "wb");
    assert(f);
    std::fwrite(file_buf.data(), 1, file_buf.size(), f);
    std::fclose(f);

    JournalReadResult result2;
    bool ok = JournalReader::readSegment(path, result2);
    assert(ok);
    assert(result2.valid);
    assert(result2.records.size() == 1);
    assert(result2.records[0].type == RecordType::ModuleDef);

    std::uint32_t module_id;
    std::string module_path;
    assert(result2.records[0].asModuleDef(module_id, module_path));
    assert(module_id == 5);
    assert(module_path == "/usr/lib/libc.so");
    std::cout << "testModuleDefRoundTrip: PASS\n";
}

void testSampleRoundTrip()
{
    Sample sample;
    sample.thread_id = 12345;
    sample.tick_id = 67;
    sample.window = 3;
    sample.weight = 4000;
    sample.thread_name = "Server thread";
    sample.frames.push_back({.module = 1, .rva = 0x1000, .raw_address = 0xABCD});
    sample.frames.push_back({.module = 2, .rva = 0x2000, .raw_address = 0xDCBA});

    JournalBuffer payload = buildSamplePayload(sample);
    auto record = serializeRecord(RecordType::Sample, 10, payload);
    auto header = serializeFileHeader(1, 0, 0);

    std::vector<std::uint8_t> file_buf;
    file_buf.insert(file_buf.end(), header.begin(), header.end());
    file_buf.insert(file_buf.end(), record.begin(), record.end());

    auto dir = makeTempDir();
    auto path = dir / "segment-0.jnl";
    std::FILE *f = std::fopen(path.string().c_str(), "wb");
    assert(f);
    std::fwrite(file_buf.data(), 1, file_buf.size(), f);
    std::fclose(f);

    JournalReadResult result;
    bool ok = JournalReader::readSegment(path, result);
    assert(ok);
    assert(result.records.size() == 1);
    assert(result.records[0].type == RecordType::Sample);

    std::uint64_t thread_id, tick_id, weight;
    std::int32_t window;
    std::vector<FrameKey> frames;
    assert(result.records[0].asSample(thread_id, tick_id, window, weight, frames));
    assert(thread_id == 12345);
    assert(tick_id == 67);
    assert(window == 3);
    assert(weight == 4000);
    assert(frames.size() == 2);
    assert(frames[0].module == 1);
    assert(frames[0].rva == 0x1000);
    assert(frames[1].module == 2);
    assert(frames[1].rva == 0x2000);
    std::cout << "testSampleRoundTrip: PASS\n";
}

void testWriterBasic()
{
    auto dir = makeTempDir();
    RecoveryWriter::Config cfg;
    cfg.directory = dir / "session-1";
    cfg.session_id = 99;
    cfg.flush_interval_ms = 50;
    cfg.sync_interval_ms = 50;

    RecoveryWriter writer(cfg);
    if (!writer.start()) {
        std::cerr << "testWriterBasic: FAIL - writer.start() returned false\n";
        std::exit(1);
    }

    writer.journalModuleDef(1, "/lib/libfoo.so");
    writer.journalModuleDef(2, "/lib/libbar.so");
    writer.journalThreadDef(100, 200, "Server thread");
    writer.journalTickEvent(0, 5.0);
    writer.journalTickEvent(1, 50.0);
    writer.journalStallBegin(1000, 500);
    writer.journalStallEnd(6000, 7000);
    writer.journalCleanEnd();

    // Wait for writer to process.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.stop();

    if (writer.writtenRecords() != 8) {
        std::cerr << "testWriterBasic: FAIL - expected 8 written, got " << writer.writtenRecords() << "\n";
        std::exit(1);
    }
    if (writer.droppedRecords() != 0) {
        std::cerr << "testWriterBasic: FAIL - expected 0 dropped, got " << writer.droppedRecords() << "\n";
        std::exit(1);
    }

    // Read back.
    auto result = JournalReader::readSession(cfg.directory);
    if (!result.valid) {
        std::cerr << "testWriterBasic: FAIL - readSession returned invalid\n";
        std::exit(1);
    }
    if (result.session_id != 99) {
        std::cerr << "testWriterBasic: FAIL - session_id=" << result.session_id << "\n";
        std::exit(1);
    }
    if (!result.has_clean_end) {
        std::cerr << "testWriterBasic: FAIL - no clean end\n";
        std::exit(1);
    }
    if (result.records.size() != 8) {
        std::cerr << "testWriterBasic: FAIL - expected 8 records, got " << result.records.size() << "\n";
        std::exit(1);
    }
    if (result.records[0].type != RecordType::ModuleDef) {
        std::cerr << "testWriterBasic: FAIL - record 0\n";
        std::exit(1);
    }
    if (result.records[1].type != RecordType::ModuleDef) {
        std::cerr << "testWriterBasic: FAIL - record 1\n";
        std::exit(1);
    }
    if (result.records[2].type != RecordType::ThreadDef) {
        std::cerr << "testWriterBasic: FAIL - record 2\n";
        std::exit(1);
    }
    if (result.records[3].type != RecordType::TickEvent) {
        std::cerr << "testWriterBasic: FAIL - record 3\n";
        std::exit(1);
    }
    if (result.records[4].type != RecordType::TickEvent) {
        std::cerr << "testWriterBasic: FAIL - record 4\n";
        std::exit(1);
    }
    if (result.records[5].type != RecordType::StallBegin) {
        std::cerr << "testWriterBasic: FAIL - record 5\n";
        std::exit(1);
    }
    if (result.records[6].type != RecordType::StallEnd) {
        std::cerr << "testWriterBasic: FAIL - record 6\n";
        std::exit(1);
    }
    if (result.records[7].type != RecordType::CleanEnd) {
        std::cerr << "testWriterBasic: FAIL - record 7\n";
        std::exit(1);
    }
    std::cout << "testWriterBasic: PASS\n";
}

void testWriterQueueDrop()
{
    auto dir = makeTempDir();
    RecoveryWriter::Config cfg;
    cfg.directory = dir / "session-2";
    cfg.session_id = 1;
    cfg.flush_interval_ms = 100000;  // very long - don't flush during test
    cfg.sync_interval_ms = 100000;
    cfg.queue_capacity = 10;

    RecoveryWriter writer(cfg);
    assert(writer.start());

    // Enqueue more than capacity.
    for (int i = 0; i < 100; ++i) {
        writer.journalTickEvent(static_cast<std::uint64_t>(i), static_cast<double>(i));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    writer.stop();

    assert(writer.droppedRecords() > 0);
    // Written records should be at most queue_capacity.
    assert(writer.writtenRecords() <= 10);
    std::cout << "testWriterQueueDrop: PASS (dropped=" << writer.droppedRecords()
              << ", written=" << writer.writtenRecords() << ")\n";
}

void testTruncationRecovery()
{
    // Write a valid file, then truncate it mid-record.
    auto dir = makeTempDir();
    auto path = dir / "segment-0.jnl";

    auto header = serializeFileHeader(1, 0, 0);
    JournalBuffer payload = buildTickEventPayload(1, 10.0);
    auto record = serializeRecord(RecordType::TickEvent, 0, payload);

    std::vector<std::uint8_t> file_buf;
    file_buf.insert(file_buf.end(), header.begin(), header.end());
    file_buf.insert(file_buf.end(), record.begin(), record.end());
    // Add a partial record (just the header, no payload).
    file_buf.push_back(static_cast<std::uint8_t>(RecordType::TickEvent));
    file_buf.push_back(0);                            // reserved
    file_buf.insert(file_buf.end(), {1, 0, 0, 0});    // sequence
    file_buf.insert(file_buf.end(), {100, 0, 0, 0});  // payload_len
    // No CRC, no payload - truncated.

    std::FILE *f = std::fopen(path.string().c_str(), "wb");
    assert(f);
    std::fwrite(file_buf.data(), 1, file_buf.size(), f);
    std::fclose(f);

    JournalReadResult result;
    bool ok = JournalReader::readSegment(path, result);
    assert(ok);
    assert(result.records.size() == 1);  // only the complete record
    assert(result.truncated_records == 1);
    std::cout << "testTruncationRecovery: PASS\n";
}

void testCorruptCRC()
{
    auto dir = makeTempDir();
    auto path = dir / "segment-0.jnl";

    auto header = serializeFileHeader(1, 0, 0);
    JournalBuffer payload = buildTickEventPayload(1, 10.0);
    auto record = serializeRecord(RecordType::TickEvent, 0, payload);

    // Corrupt the CRC (flip a bit in the payload).
    record[kRecordHeaderSize] ^= 0xFF;

    std::vector<std::uint8_t> file_buf;
    file_buf.insert(file_buf.end(), header.begin(), header.end());
    file_buf.insert(file_buf.end(), record.begin(), record.end());

    std::FILE *f = std::fopen(path.string().c_str(), "wb");
    assert(f);
    std::fwrite(file_buf.data(), 1, file_buf.size(), f);
    std::fclose(f);

    JournalReadResult result;
    bool ok = JournalReader::readSegment(path, result);
    assert(ok);
    assert(result.records.empty());
    assert(result.corrupt_records == 1);
    std::cout << "testCorruptCRC: PASS\n";
}

void testWriterStopJoins()
{
    auto dir = makeTempDir();
    RecoveryWriter::Config cfg;
    cfg.directory = dir / "session-3";
    cfg.session_id = 1;
    cfg.flush_interval_ms = 1000;

    {
        RecoveryWriter writer(cfg);
        assert(writer.start());
        writer.journalTickEvent(0, 1.0);
    }  // destructor calls stop()
    // If the thread wasn't joined, this would be UB.
    std::cout << "testWriterStopJoins: PASS\n";
}

void writeSegment(const std::filesystem::path &path, std::uint64_t session_id, std::uint32_t segment_number,
                  std::uint32_t sequence, RecordType type, const JournalBuffer &payload)
{
    auto header = serializeFileHeader(session_id, session_id, segment_number);
    auto record = serializeRecord(type, sequence, payload);
    std::FILE *file = std::fopen(path.string().c_str(), "wb");
    assert(file);
    assert(std::fwrite(header.data(), 1, header.size(), file) == header.size());
    assert(std::fwrite(record.data(), 1, record.size(), file) == record.size());
    std::fclose(file);
}

struct RecordSpec {
    RecordType type;
    std::uint32_t sequence;
    JournalBuffer payload;
};

void writeSegmentMulti(const std::filesystem::path &path, std::uint64_t session_id, std::uint32_t segment_number,
                       const std::vector<RecordSpec> &records)
{
    auto header = serializeFileHeader(session_id, session_id, segment_number);
    std::FILE *file = std::fopen(path.string().c_str(), "wb");
    assert(file);
    assert(std::fwrite(header.data(), 1, header.size(), file) == header.size());
    for (const auto &rec : records) {
        auto record = serializeRecord(rec.type, rec.sequence, rec.payload);
        assert(std::fwrite(record.data(), 1, record.size(), file) == record.size());
    }
    std::fclose(file);
}

void testSessionIsolation()
{
    auto dir = makeTempDir() / "session-isolation";
    std::filesystem::create_directories(dir);

    writeSegment(dir / "segment-0.jnl", 200, 0, 0, RecordType::TickEvent, buildTickEventPayload(1, 5.0));
    writeSegment(dir / "segment-1.jnl", 100, 1, 1, RecordType::CleanEnd, buildCleanEndPayload(1));

    auto result = JournalReader::readSession(dir);
    assert(result.valid);
    assert(result.session_id == 200);
    assert(result.records.size() == 1);
    assert(!result.has_clean_end);

    RecoveryWriter::Config config;
    config.directory = dir;
    config.session_id = 300;
    RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalTickEvent(2, 6.0);
    writer.stop();

    assert(std::filesystem::exists(dir / "segment-0.jnl"));
    assert(!std::filesystem::exists(dir / "segment-1.jnl"));
    result = JournalReader::readSession(dir);
    assert(result.valid);
    assert(result.session_id == 300);
    assert(result.records.size() == 1);
    assert(!result.has_clean_end);
    std::cout << "testSessionIsolation: PASS\n";
}

void testSessionConfigRoundTrip()
{
    std::vector<std::string> patterns = {"Server thread", "Worker-*"};
    JournalBuffer payload =
        buildSessionConfigPayload(8000, 50, true, false, true, 2, 1, true, "Console", false, "test profile", patterns);
    auto record = serializeRecord(RecordType::SessionConfig, 0, payload);

    JournalRecord rec;
    rec.type = RecordType::SessionConfig;
    rec.payload.assign(record.begin() + kRecordHeaderSize, record.end());

    SessionConfig sc;
    assert(rec.asSessionConfig(sc));
    assert(sc.present);
    assert(sc.interval_us == 8000);
    assert(sc.only_ticks_over_ms == 50);
    assert(sc.all_threads);
    assert(!sc.regex_threads);
    assert(sc.ignore_sleeping);
    assert(sc.thread_grouper == 2);
    assert(sc.profile_type == 1);
    assert(sc.live_only);
    assert(sc.creator_name == "Console");
    assert(!sc.creator_is_player);
    assert(sc.comment == "test profile");
    assert(sc.thread_patterns.size() == 2);
    assert(sc.thread_patterns[0] == "Server thread");
    assert(sc.thread_patterns[1] == "Worker-*");
    std::cout << "testSessionConfigRoundTrip: PASS\n";
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
                                {"Server thread"});
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
    // No journalCleanEnd() - this is a full replay test, not a clean-end test.

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

    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, "clean stop", {});
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
    // Clean-end sessions skip expensive replay; no profile is built.
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

    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, "crashed", {});
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 100, "Server thread");

    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writer.journalSample(sample);
    // No journalCleanEnd() - simulate crash.

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

    writer.journalSessionConfig(524287, 0, true, false, false, 1, 1, true, "Console", false, "live-only", {});
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

// Test 1: Clean-end sessions must skip expensive replay/symbolication entirely.
// A clean-end journal that references a missing module must still return
// valid=true with has_clean_end=true, proving replay did not touch the data.
void testCleanEndEarlyExit()
{
    auto dir = makeTempDir() / "clean-early-exit";
    std::filesystem::create_directories(dir);

    // Sample references module 99, but no ModuleDef 99 exists.  If replay
    // proceeded, this would be "missing module id 99".
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
          .payload = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {})},
         {.type = RecordType::Sample, .sequence = 1, .payload = buildSamplePayload(sample)},
         {.type = RecordType::CleanEnd, .sequence = 2, .payload = buildCleanEndPayload(1)}});

    auto result = RecoveryPlayer::replay(dir);
    assert(result.valid);
    assert(result.has_clean_end);
    assert(result.serialized_proto.empty());  // no replay work done
    std::cout << "testCleanEndEarlyExit: PASS\n";
}

// Test 2: Head-truncated journal (missing segment 0) must not replay.
void testHeadTruncatedJournal()
{
    auto dir = makeTempDir() / "head-truncated";
    std::filesystem::create_directories(dir);

    Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});

    // Only segment-3 exists; segment-0..2 were rotated away.
    writeSegmentMulti(
        dir / "segment-3.jnl", 600000, 3,
        {{.type = RecordType::ModuleDef, .sequence = 0, .payload = buildModuleDefPayload(0, "bedrock_server")},
         {.type = RecordType::Sample, .sequence = 1, .payload = buildSamplePayload(sample)}});

    auto result = RecoveryPlayer::replay(dir);
    assert(!result.valid);
    assert(result.error.find("head-truncated") != std::string::npos);

    // Also verify the reader exposes head_truncated.
    auto journal = JournalReader::readSession(dir);
    assert(journal.valid);
    assert(journal.head_truncated);
    assert(journal.first_segment_number == 3);
    std::cout << "testHeadTruncatedJournal: PASS\n";
}

// Test 3: Non-contiguous recorded ModuleId must be remapped to local IDs.
void testNonContiguousModuleId()
{
    auto dir = makeTempDir() / "noncontiguous-modid";
    std::filesystem::create_directories(dir);

    // ModuleDef recorded id = 20 (the only module).  Local intern() assigns 0.
    // Sample frame.module = 20 must be remapped to local 0 before symbolication.
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
          .payload = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {})},
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

// Test 4: Sample referencing a missing ModuleDef must fail safely.
void testMissingModuleDefReferenced()
{
    auto dir = makeTempDir() / "missing-moddef";
    std::filesystem::create_directories(dir);

    // No ModuleDef for module id 7.
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
          .payload = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {})},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(0, "bedrock_server")},
         {.type = RecordType::ThreadDef, .sequence = 2, .payload = buildThreadDefPayload(1, 100, "Server thread")},
         {.type = RecordType::Sample, .sequence = 3, .payload = buildSamplePayload(sample)}});

    auto result = RecoveryPlayer::replay(dir);
    assert(!result.valid);
    assert(result.error.find("missing module id 7") != std::string::npos);
    std::cout << "testMissingModuleDefReferenced: PASS\n";
}

// Test 5: Rolling journal with rotation must retain metadata via a snapshot
// and remain recoverable after segment-0 is pruned.
void testRollingJournalRecovery()
{
    auto dir = makeTempDir() / "rolling";
    std::filesystem::create_directories(dir);

    RecoveryWriter::Config cfg;
    cfg.directory = dir;
    cfg.session_id = 900000;
    cfg.max_segment_bytes = 512;  // tiny: forces rotation after a few records
    cfg.max_total_bytes = 1024;   // tiny: forces pruning of segment-0
    cfg.flush_interval_ms = 20;
    cfg.sync_interval_ms = 20;

    RecoveryWriter writer(cfg);
    if (!writer.start()) {
        std::cerr << "testRollingJournalRecovery: FAIL - writer.start() returned false\n";
        std::exit(1);
    }

    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, "rolling test", {});
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalModuleDef(1, "libfoo.so");
    writer.journalThreadDef(100, 200, "Server thread");

    // Write enough samples to trigger multiple rotations and prune segment-0.
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

    // segment-0 must have been pruned.
    if (std::filesystem::exists(dir / "segment-0.jnl")) {
        std::cerr << "testRollingJournalRecovery: FAIL - segment-0 still exists\n";
        std::exit(1);
    }
    // A metadata snapshot must exist.
    if (!std::filesystem::exists(dir / "metadata.snapshot")) {
        std::cerr << "testRollingJournalRecovery: FAIL - no metadata.snapshot\n";
        std::exit(1);
    }

    // The reader must NOT report head_truncated (snapshot covers pruned metadata).
    auto journal = JournalReader::readSession(dir);
    assert(journal.valid);
    assert(!journal.head_truncated);
    if (!journal.metadata_snapshot.has_value()) {
        std::cerr << "testRollingJournalRecovery: FAIL - snapshot was not loaded\n";
        std::exit(1);
    }
    const MetadataSnapshot &snapshot = journal.metadata_snapshot.value();
    assert(snapshot.valid);
    assert(snapshot.session_id == 900000);

    // Replay must succeed and produce a non-empty protobuf.
    auto result = RecoveryPlayer::replay(dir);
    if (!result.valid) {
        std::cerr << "testRollingJournalRecovery: FAIL - " << result.error << "\n";
        std::exit(1);
    }
    assert(!result.has_clean_end);
    assert(result.sample_count > 0);
    assert(result.thread_count >= 1);
    assert(result.session_start_ms > 900000);
    assert(result.tick_count > 0);
    assert(result.tick_count < 200);
    assert(!result.serialized_proto.empty());
    std::cout << "testRollingJournalRecovery: PASS (samples=" << result.sample_count
              << ", proto=" << result.serialized_proto.size() << ")\n";
}

// Test 6: A corrupt snapshot (wrong session id) must not enable recovery; the
// journal is treated as head-truncated and replay returns invalid.
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

    RecoveryWriter writer(cfg);
    assert(writer.start());
    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {});
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

    // Overwrite the snapshot with one that has a different session id.
    std::vector<SnapshotModuleDef> modules{{.module_id = 0, .path = "bedrock_server"}};
    std::vector<SnapshotThreadDef> threads{{.thread_id = 1, .os_thread_id = 100, .name = "Server thread"}};
    auto buf = serializeMetadataSnapshot(999999, 0, {}, modules, threads);
    std::FILE *f = std::fopen((dir / "metadata.snapshot").string().c_str(), "wb");
    assert(f);
    assert(std::fwrite(buf.data(), 1, buf.size(), f) == buf.size());
    std::fclose(f);

    auto result = RecoveryPlayer::replay(dir);
    assert(!result.valid);
    assert(result.error.find("head-truncated") != std::string::npos);
    std::cout << "testCorruptSnapshotWrongSession: PASS\n";
}

// Test 7: A truncated snapshot (shorter than the header) must be treated as
// absent, leaving the head-truncated journal unrecoverable.
void testTruncatedSnapshot()
{
    auto dir = makeTempDir() / "truncated-snapshot";
    std::filesystem::create_directories(dir);

    // Write a segment-3 (head-truncated) and a truncated snapshot.
    Sample sample;
    sample.thread_id = 1;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writeSegmentMulti(dir / "segment-3.jnl", 920000, 3,
                      {{.type = RecordType::Sample, .sequence = 0, .payload = buildSamplePayload(sample)}});

    std::FILE *f = std::fopen((dir / "metadata.snapshot").string().c_str(), "wb");
    assert(f);
    std::vector<std::uint8_t> partial{'S', 'P', 'R', 'K', 'M', 'E', 'T', 'A'};  // magic only
    assert(std::fwrite(partial.data(), 1, partial.size(), f) == partial.size());
    std::fclose(f);

    auto journal = JournalReader::readSession(dir);
    assert(journal.valid);
    assert(journal.head_truncated);  // snapshot invalid -> stays head-truncated

    auto result = RecoveryPlayer::replay(dir);
    assert(!result.valid);
    std::cout << "testTruncatedSnapshot: PASS\n";
}

// Test 8: Allocation-style ModuleDef(0, "<other modules>") must allow recovery
// of samples whose frames were assigned to the sentinel module.
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
          .payload = buildSessionConfigPayload(524287, 0, true, false, false, 1, 1, false, "Console", false, {}, {})},
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

// Test 9: A sample referencing module 0 when no ModuleDef 0 exists must fail
// safely (sentinel was never journaled).
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

    // ModuleDef 1 exists but ModuleDef 0 (sentinel) does not.
    writeSegmentMulti(
        dir / "segment-0.jnl", 940000, 0,
        {{.type = RecordType::SessionConfig,
          .sequence = 0,
          .payload = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {})},
         {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(1, "bedrock_server")},
         {.type = RecordType::ThreadDef, .sequence = 2, .payload = buildThreadDefPayload(1, 100, "Server thread")},
         {.type = RecordType::Sample, .sequence = 3, .payload = buildSamplePayload(sample)}});

    auto result = RecoveryPlayer::replay(dir);
    assert(!result.valid);
    assert(result.error.find("missing module id 0") != std::string::npos);
    std::cout << "testMissingSentinelModule0: PASS\n";
}

// Test 10: Stopping the writer without journalCleanEnd must leave a journal
// that is recoverable (simulates crash after sampling stops but before export).
void testStopWithoutCleanEndRecoverable()
{
    auto dir = makeTempDir() / "stop-no-cleanend";
    std::filesystem::create_directories(dir);

    RecoveryWriter::Config cfg;
    cfg.directory = dir;
    cfg.session_id = 950000;
    cfg.flush_interval_ms = 50;
    cfg.sync_interval_ms = 50;

    RecoveryWriter writer(cfg);
    assert(writer.start());
    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {});
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
    // No journalCleanEnd() - simulates stopSampling() without export commit.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.stop();

    auto result = RecoveryPlayer::replay(dir);
    assert(result.valid);
    assert(!result.has_clean_end);
    assert(result.sample_count == 1);
    assert(!result.serialized_proto.empty());
    std::cout << "testStopWithoutCleanEndRecoverable: PASS\n";
}

void writeBytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
    std::FILE *file = std::fopen(path.string().c_str(), "wb");
    assert(file);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    std::fclose(file);
}

void testSnapshotRejectsMaliciousCountAndTrailingGarbage()
{
    auto dir = makeTempDir() / "snapshot-validation";
    std::filesystem::create_directories(dir);

    auto bytes = serializeMetadataSnapshot(960000, 0, {}, {}, {});
    const std::uint32_t malicious_count = std::numeric_limits<std::uint32_t>::max();
    std::memcpy(bytes.data() + kSnapshotHeaderSize + sizeof(std::uint16_t), &malicious_count, sizeof(malicious_count));
    const auto payload_size = static_cast<std::uint32_t>(bytes.size() - kSnapshotHeaderSize);
    const auto crc = static_cast<std::uint32_t>(crc32(0L, bytes.data() + kSnapshotHeaderSize, payload_size));
    constexpr std::size_t k_snapshot_crc_offset = kSnapshotHeaderSize - sizeof(std::uint32_t);
    std::memcpy(bytes.data() + k_snapshot_crc_offset, &crc, sizeof(crc));
    writeBytes(dir / "metadata.snapshot", bytes);
    assert(!JournalReader::readMetadataSnapshot(dir).valid);

    bytes = serializeMetadataSnapshot(960000, 0, {}, {}, {});
    bytes.push_back(0xff);
    writeBytes(dir / "metadata.snapshot", bytes);
    assert(!JournalReader::readMetadataSnapshot(dir).valid);
    std::cout << "testSnapshotRejectsMaliciousCountAndTrailingGarbage: PASS\n";
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
    writeBytes(dir / "metadata.snapshot", serializeMetadataSnapshot(970000, 0, {}, modules, threads));

    const auto result = RecoveryPlayer::replay(dir);
    assert(result.valid);
    assert(result.thread_count == 1);
    assert(result.tick_count == 1);
    assert(result.session_start_ms == 1'270'000);
    std::cout << "testSnapshotOnlyThreadIsNotExported: PASS\n";
}

void testDuplicateSequenceRejected()
{
    auto dir = makeTempDir() / "duplicate-sequence";
    std::filesystem::create_directories(dir);
    Sample sample;
    sample.thread_id = 1;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writeSegmentMulti(
        dir / "segment-0.jnl", 980000, 0,
        {{.type = RecordType::ModuleDef, .sequence = 7, .payload = buildModuleDefPayload(0, "bedrock_server")},
         {.type = RecordType::Sample, .sequence = 7, .payload = buildSamplePayload(sample)}});
    const auto journal = JournalReader::readSession(dir);
    assert(journal.duplicate_sequences);
    const auto result = RecoveryPlayer::replay(dir);
    assert(!result.valid);
    assert(result.error.find("duplicate") != std::string::npos);
    std::cout << "testDuplicateSequenceRejected: PASS\n";
}

}  // namespace

int main()
{
    testFileHeaderMagic();
    testRecordSerialization();
    testModuleDefRoundTrip();
    testSampleRoundTrip();
    testWriterBasic();
    testWriterQueueDrop();
    testTruncationRecovery();
    testCorruptCRC();
    testWriterStopJoins();
    testSessionIsolation();
    testSessionConfigRoundTrip();
    testRecoveryPlayerReplay();
    testRecoveryPlayerEmptyJournal();
    testCleanEndDetected();
    testNoCleanEndRecovered();
    testLiveOnlyRefused();
    testCleanEndEarlyExit();
    testHeadTruncatedJournal();
    testNonContiguousModuleId();
    testMissingModuleDefReferenced();
    testRollingJournalRecovery();
    testCorruptSnapshotWrongSession();
    testTruncatedSnapshot();
    testAllocationSentinelModule0();
    testMissingSentinelModule0();
    testStopWithoutCleanEndRecoverable();
    testSnapshotRejectsMaliciousCountAndTrailingGarbage();
    testSnapshotOnlyThreadIsNotExported();
    testDuplicateSequenceRejected();
    std::cout << "All journal tests passed.\n";
    return 0;
}
