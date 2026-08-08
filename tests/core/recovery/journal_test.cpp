#include "core/recovery/journal_format.h"
#include "core/recovery/journal_reader.h"
#include "core/recovery/recovery_player.h"
#include "core/recovery/recovery_writer.h"
#include "native/sampler/types.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace spark;

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
    sample.frames.push_back({1, 0x1000, 0xABCD});
    sample.frames.push_back({2, 0x2000, 0xDCBA});

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
        std::cerr << "testWriterBasic: FAIL - expected 8 written, got "
                  << writer.writtenRecords() << "\n";
        std::exit(1);
    }
    if (writer.droppedRecords() != 0) {
        std::cerr << "testWriterBasic: FAIL - expected 0 dropped, got "
                  << writer.droppedRecords() << "\n";
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
        std::cerr << "testWriterBasic: FAIL - expected 8 records, got "
                  << result.records.size() << "\n";
        std::exit(1);
    }
    if (result.records[0].type != RecordType::ModuleDef) { std::cerr << "testWriterBasic: FAIL - record 0\n"; std::exit(1); }
    if (result.records[1].type != RecordType::ModuleDef) { std::cerr << "testWriterBasic: FAIL - record 1\n"; std::exit(1); }
    if (result.records[2].type != RecordType::ThreadDef) { std::cerr << "testWriterBasic: FAIL - record 2\n"; std::exit(1); }
    if (result.records[3].type != RecordType::TickEvent) { std::cerr << "testWriterBasic: FAIL - record 3\n"; std::exit(1); }
    if (result.records[4].type != RecordType::TickEvent) { std::cerr << "testWriterBasic: FAIL - record 4\n"; std::exit(1); }
    if (result.records[5].type != RecordType::StallBegin) { std::cerr << "testWriterBasic: FAIL - record 5\n"; std::exit(1); }
    if (result.records[6].type != RecordType::StallEnd) { std::cerr << "testWriterBasic: FAIL - record 6\n"; std::exit(1); }
    if (result.records[7].type != RecordType::CleanEnd) { std::cerr << "testWriterBasic: FAIL - record 7\n"; std::exit(1); }
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
    file_buf.push_back(0);  // reserved
    file_buf.insert(file_buf.end(), {1, 0, 0, 0});  // sequence
    file_buf.insert(file_buf.end(), {100, 0, 0, 0}); // payload_len
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

void testSessionConfigRoundTrip()
{
    std::vector<std::string> patterns = {"Server thread", "Worker-*"};
    JournalBuffer payload = buildSessionConfigPayload(
        8000, 50, true, false, true, 2, "Console", false, "test profile", patterns);
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

    writer.journalSessionConfig(4000, 0, false, false, false, 1,
                                "Console", false, "replay test", {"Server thread"});
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(100, 200, "Server thread");

    Sample sample;
    sample.thread_id = 100;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({0, 0x1000, 0});
    sample.frames.push_back({0, 0x2000, 0});
    writer.journalSample(sample);

    sample.tick_id = 1;
    sample.window = 1;
    writer.journalSample(sample);

    writer.journalTickEvent(0, 5.0);
    writer.journalTickEvent(1, 10.0);
    writer.journalCleanEnd();

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
    assert(result.has_clean_end);
    assert(!result.serialized_proto.empty());
    std::cout << "testRecoveryPlayerReplay: PASS (proto size="
              << result.serialized_proto.size() << ")\n";
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
    testSessionConfigRoundTrip();
    testRecoveryPlayerReplay();
    testRecoveryPlayerEmptyJournal();
    std::cout << "All journal tests passed.\n";
    return 0;
}
