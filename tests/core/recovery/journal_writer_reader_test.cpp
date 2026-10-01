#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

#include "core/recovery/journal_reader.h"
#include "core/recovery/recovery_writer.h"
#include "journal_test_cases.h"
#include "journal_test_support.h"

using namespace spark;                // NOLINT(google-build-using-namespace)
using namespace spark::journal_test;  // NOLINT(google-build-using-namespace)

void testWriterBasic()
{
    auto dir = makeTempDir();
    RecoveryWriter::Config cfg;
    cfg.directory = dir / "session-1";
    cfg.session_id = 99;
    cfg.flush_interval_ms = 50;
    cfg.sync_interval_ms = 50;
    cfg.shutdown_timeout_ms = 15000;

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
    assert(result.records[0].type == RecordType::ModuleDef);
    assert(result.records[1].type == RecordType::ModuleDef);
    assert(result.records[2].type == RecordType::ThreadDef);
    assert(result.records[3].type == RecordType::TickEvent);
    assert(result.records[4].type == RecordType::TickEvent);
    assert(result.records[5].type == RecordType::StallBegin);
    assert(result.records[6].type == RecordType::StallEnd);
    assert(result.records[7].type == RecordType::CleanEnd);
    std::cout << "testWriterBasic: PASS\n";
}

void testWriterStopJoins()
{
    auto dir = makeTempDir();
    RecoveryWriter::Config cfg;
    cfg.directory = dir / "session-3";
    cfg.session_id = 1;
    cfg.flush_interval_ms = 1000;
    cfg.shutdown_timeout_ms = 15000;

    {
        RecoveryWriter writer(cfg);
        assert(writer.start());
        writer.journalTickEvent(0, 1.0);
    }
    std::cout << "testWriterStopJoins: PASS\n";
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
    config.shutdown_timeout_ms = 15000;
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

void testUnsupportedAndMixedVersionsRejected()
{
    auto unsupported = makeTempDir() / "unsupported-version";
    std::filesystem::create_directories(unsupported);
    writeSegment(unsupported / "segment-0.jnl", 1, 0, 0, RecordType::TickEvent, buildTickEventPayload(0, 1.0), 1);
    assert(!JournalReader::readSession(unsupported).valid);

    auto mixed = makeTempDir() / "mixed-version";
    std::filesystem::create_directories(mixed);
    writeSegment(mixed / "segment-0.jnl", 2, 0, 0, RecordType::TickEvent, buildTickEventPayload(0, 1.0));
    writeSegment(mixed / "segment-1.jnl", 2, 1, 1, RecordType::TickEvent, buildTickEventPayload(1, 1.0),
                 kLegacyJournalVersion);
    const auto result = JournalReader::readSession(mixed);
    assert(!result.valid);
    assert(result.version == kJournalVersion);
    std::cout << "testUnsupportedAndMixedVersionsRejected: PASS\n";
}
