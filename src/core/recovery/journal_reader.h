#ifndef SPARK_CORE_RECOVERY_JOURNAL_READER_H
#define SPARK_CORE_RECOVERY_JOURNAL_READER_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/recovery/journal_format.h"
#include "native/sampler/types.h"

namespace spark {

// Session configuration parsed from a SessionConfig journal record.
struct SessionConfig {
    bool present = false;
    std::uint32_t interval_us = 4000;
    std::int32_t only_ticks_over_ms = 0;
    bool all_threads = false;
    bool regex_threads = false;
    bool ignore_sleeping = false;
    std::uint8_t thread_grouper = 1;  // ByPool
    std::string creator_name = "Console";
    bool creator_is_player = false;
    std::string comment;
    std::vector<std::string> thread_patterns;
};

// A parsed journal record.
struct JournalRecord {
    RecordType type;
    std::uint32_t sequence;
    std::vector<std::uint8_t> payload;

    // Payload accessors (return false on short/invalid payload).
    bool asModuleDef(std::uint32_t &module_id, std::string &path) const;
    bool asThreadDef(std::uint64_t &thread_id, std::uint64_t &os_thread_id,
                     std::string &name) const;
    bool asSample(std::uint64_t &thread_id, std::uint64_t &tick_id,
                  std::int32_t &window, std::uint64_t &weight,
                  std::vector<FrameKey> &frames) const;
    bool asTickEvent(std::uint64_t &tick_id, double &mspt) const;
    bool asStallBegin(std::uint64_t &detected_ns, std::uint64_t &last_tick_ns) const;
    bool asStallEnd(std::uint64_t &detected_ns, std::uint64_t &recovered_ns) const;
    bool asCleanEnd(std::uint64_t &timestamp_ns) const;
    bool asSessionConfig(SessionConfig &config) const;
};

// Result of reading a journal session.
struct JournalReadResult {
    bool valid = false;         // at least the file header was parsed
    std::uint64_t session_id = 0;
    std::uint64_t created_ns = 0;
    bool has_clean_end = false;
    SessionConfig session_config;
    std::vector<JournalRecord> records;
    std::uint64_t corrupt_records = 0;  // CRC mismatches
    std::uint64_t truncated_records = 0; // incomplete trailing records
};

// Reads all segment files from a recovery directory, parses records, and
// validates CRC.  Truncated trailing records are silently dropped.  Records
// with CRC mismatches terminate reading of the current segment (the writer
// never produces out-of-order records, so a CRC failure indicates corruption
// or an interrupted write).
class JournalReader {
public:
    // Reads all segments in the given directory, ordered by segment number.
    static JournalReadResult readSession(const std::filesystem::path &directory);

    // Reads a single segment file.  Exposed for testing.
    static bool readSegment(const std::filesystem::path &path, JournalReadResult &result);
};

}  // namespace spark

#endif  // SPARK_CORE_RECOVERY_JOURNAL_READER_H
