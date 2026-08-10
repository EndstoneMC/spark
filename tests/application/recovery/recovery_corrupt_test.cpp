#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "application/spark_application.h"
#include "core/recovery/journal_format.h"
#include "core/recovery/recovery_writer.h"
#include "native/sampler/types.h"

namespace {

class TestDispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override { task(); }
};

class TestMetadataProvider final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ExportContext & /*ctx*/, std::int64_t /*now_ms*/) override {}
    void gatherWorldMetadata(spark::ExportContext & /*ctx*/) override {}
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }
};

class TestNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string & /*sender_name*/, const std::string &text) override { messages.push_back(text); }
    std::vector<std::string> messages;
};

// Notifier whose notify() throws.  Used to verify the ABI firewall: recovery
// must never let the exception escape the Spark DSO.
class ThrowingNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string & /*sender_name*/, const std::string & /*text*/) override
    {
        throw std::runtime_error("notifier exploded");
    }
};

struct RecordSpec {
    spark::RecordType type;
    std::uint32_t sequence;
    spark::JournalBuffer payload;
};

void writeSegmentMulti(const std::filesystem::path &path, std::uint64_t session_id, std::uint32_t segment_number,
                       const std::vector<RecordSpec> &records)
{
    auto header = spark::serializeFileHeader(session_id, session_id, segment_number);
    std::FILE *file = std::fopen(path.string().c_str(), "wb");
    assert(file);
    assert(std::fwrite(header.data(), 1, header.size(), file) == header.size());
    for (const auto &rec : records) {
        auto record = spark::serializeRecord(rec.type, rec.sequence, rec.payload);
        assert(std::fwrite(record.data(), 1, record.size(), file) == record.size());
    }
    std::fclose(file);
}

// Returns true if enable() completed without throwing and the recovery
// directory no longer contains segment files.
bool runEnableWithRecovery(const std::filesystem::path &root, const std::filesystem::path &recovery,
                           TestNotifier &notifier)
{
    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    spark::SparkApplication application({}, root, root / "activity.json", std::move(config), std::move(trusted),
                                        dispatcher, metadata, notifier);
    try {
        application.enable();
    }
    catch (const std::exception &e) {
        std::fprintf(stderr, "enable() threw: %s\n", e.what());
        return false;
    }
    catch (...) {
        std::fprintf(stderr, "enable() threw unknown exception\n");
        return false;
    }
    application.shutdown();

    // The recovery directory must not retain segment files (they were cleaned
    // or quarantined), so the next startup does not re-read the same bad data.
    bool has_segment = false;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(recovery, ec)) {
        if (ec) {
            break;
        }
        auto name = entry.path().filename().string();
        if (name.starts_with("segment-") && name.ends_with(".jnl")) {
            has_segment = true;
            break;
        }
    }
    return !has_segment;
}

void testHeadTruncatedRecovery()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_recovery_corrupt_head";
    std::filesystem::remove_all(root);
    const auto recovery = root / "recovery";
    std::filesystem::create_directories(recovery);

    // Only segment-3; segment-0..2 rotated away.  Sample references module 0
    // whose ModuleDef was in the deleted early segments.
    spark::Sample sample;
    sample.thread_id = 1;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writeSegmentMulti(
        recovery / "segment-3.jnl", 900000, 3,
        {{.type = spark::RecordType::ModuleDef,
          .sequence = 0,
          .payload = spark::buildModuleDefPayload(0, "bedrock_server")},
         {.type = spark::RecordType::Sample, .sequence = 1, .payload = spark::buildSamplePayload(sample)}});

    TestNotifier notifier;
    assert(runEnableWithRecovery(root, recovery, notifier));
    assert(!notifier.messages.empty());
    std::filesystem::remove_all(root);
    std::cout << "testHeadTruncatedRecovery: PASS\n";
}

void testMissingModuleDefRecovery()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_recovery_corrupt_moddef";
    std::filesystem::remove_all(root);
    const auto recovery = root / "recovery";
    std::filesystem::create_directories(recovery);

    // Sample references module 7 but no ModuleDef 7 exists.
    spark::Sample sample;
    sample.thread_id = 1;
    sample.weight = 4000;
    sample.frames.push_back({.module = 7, .rva = 0x1000, .raw_address = 0});
    writeSegmentMulti(
        recovery / "segment-0.jnl", 910000, 0,
        {{.type = spark::RecordType::ModuleDef,
          .sequence = 0,
          .payload = spark::buildModuleDefPayload(0, "bedrock_server")},
         {.type = spark::RecordType::Sample, .sequence = 1, .payload = spark::buildSamplePayload(sample)}});

    TestNotifier notifier;
    assert(runEnableWithRecovery(root, recovery, notifier));
    assert(!notifier.messages.empty());
    std::filesystem::remove_all(root);
    std::cout << "testMissingModuleDefRecovery: PASS\n";
}

void testGarbledJournalRecovery()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_recovery_corrupt_garbled";
    std::filesystem::remove_all(root);
    const auto recovery = root / "recovery";
    std::filesystem::create_directories(recovery);

    // Write random bytes as a segment file.
    std::ofstream out(recovery / "segment-0.jnl", std::ios::binary);
    std::vector<char> garbage(256);
    for (std::size_t i = 0; i < garbage.size(); ++i) {
        garbage[i] = static_cast<char>(i * 7 + 3);
    }
    out.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    out.close();

    TestNotifier notifier;
    assert(runEnableWithRecovery(root, recovery, notifier));
    std::filesystem::remove_all(root);
    std::cout << "testGarbledJournalRecovery: PASS\n";
}

void testCleanEndRecoveryCleanedUp()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_recovery_corrupt_cleanend";
    std::filesystem::remove_all(root);
    const auto recovery = root / "recovery";
    std::filesystem::create_directories(recovery);

    spark::Sample sample;
    sample.thread_id = 1;
    sample.weight = 4000;
    sample.frames.push_back({.module = 99, .rva = 0x1000, .raw_address = 0});
    writeSegmentMulti(
        recovery / "segment-0.jnl", 920000, 0,
        {{.type = spark::RecordType::Sample, .sequence = 0, .payload = spark::buildSamplePayload(sample)},
         {.type = spark::RecordType::CleanEnd, .sequence = 1, .payload = spark::buildCleanEndPayload(1)}});

    TestNotifier notifier;
    assert(runEnableWithRecovery(root, recovery, notifier));
    std::filesystem::remove_all(root);
    std::cout << "testCleanEndRecoveryCleanedUp: PASS\n";
}

// Recovery with a notifier that throws from notify().  enable() must return
// normally; the exception must not cross the Spark DSO boundary.
void testThrowingNotifierRecovery()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_recovery_corrupt_throwing";
    std::filesystem::remove_all(root);
    const auto recovery = root / "recovery";
    std::filesystem::create_directories(recovery);

    // Head-truncated journal (segment-3, no snapshot) -> invalid -> safeNotify called -> throws.
    spark::Sample sample;
    sample.thread_id = 1;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writeSegmentMulti(
        recovery / "segment-3.jnl", 930000, 3,
        {{.type = spark::RecordType::Sample, .sequence = 0, .payload = spark::buildSamplePayload(sample)}});

    ThrowingNotifier notifier;
    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    spark::SparkApplication application({}, root, root / "activity.json", std::move(config), std::move(trusted),
                                        dispatcher, metadata, notifier);
    try {
        application.enable();
    }
    catch (...) {
        std::cerr << "testThrowingNotifierRecovery: FAIL - enable() threw\n";
        std::exit(1);
    }
    application.shutdown();

    // Segments must still be cleaned up despite the throwing notifier.
    bool has_segment = false;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(recovery, ec)) {
        if (ec) {
            break;
        }
        auto name = entry.path().filename().string();
        if (name.starts_with("segment-") && name.ends_with(".jnl")) {
            has_segment = true;
            break;
        }
    }
    assert(!has_segment);
    std::filesystem::remove_all(root);
    std::cout << "testThrowingNotifierRecovery: PASS\n";
}

// Rolling journal with pruned segment-0 and a valid metadata snapshot must
// recover successfully through SparkApplication::enable().
void testRollingSnapshotRecovery()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_recovery_corrupt_rolling";
    std::filesystem::remove_all(root);
    const auto recovery = root / "recovery";
    std::filesystem::create_directories(recovery);

    spark::RecoveryWriter::Config cfg;
    cfg.directory = recovery;
    cfg.session_id = 940000;
    cfg.max_segment_bytes = 512;
    cfg.max_total_bytes = 1024;
    cfg.flush_interval_ms = 20;
    cfg.sync_interval_ms = 20;
    spark::RecoveryWriter writer(cfg);
    assert(writer.start());
    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, "rolling recovery", {});
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 100, "Server thread");
    for (int i = 0; i < 200; ++i) {
        spark::Sample sample;
        sample.thread_id = 1;
        sample.tick_id = static_cast<std::uint64_t>(i);
        sample.window = 0;
        sample.weight = 4000;
        sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
        writer.journalSample(sample);
        writer.journalTickEvent(static_cast<std::uint64_t>(i), 5.0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    writer.stop();

    assert(!std::filesystem::exists(recovery / "segment-0.jnl"));
    assert(std::filesystem::exists(recovery / "metadata.snapshot"));

    TestNotifier notifier;
    assert(runEnableWithRecovery(root, recovery, notifier));
    assert(!notifier.messages.empty());
    // The first message should announce a recovered profile, not a discard.
    bool found_recovered = false;
    for (const auto &msg : notifier.messages) {
        if (msg.find("Recovered profile") != std::string::npos) {
            found_recovered = true;
            break;
        }
    }
    assert(found_recovered);
    std::filesystem::remove_all(root);
    std::cout << "testRollingSnapshotRecovery: PASS\n";
}

}  // namespace

int main()
{
    testHeadTruncatedRecovery();
    testMissingModuleDefRecovery();
    testGarbledJournalRecovery();
    testCleanEndRecoveryCleanedUp();
    testThrowingNotifierRecovery();
    testRollingSnapshotRecovery();
    std::cout << "All recovery corrupt tests passed.\n";
    return 0;
}
