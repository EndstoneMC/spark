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
    std::uint8_t profile_type = 0;    // 0=execution, 1=allocation
    bool live_only = false;           // allocation live-only mode
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
    bool asThreadDef(std::uint64_t &thread_id, std::uint64_t &os_thread_id, std::string &name) const;
    bool asSample(std::uint64_t &thread_id, std::uint64_t &tick_id, std::int32_t &window, std::uint64_t &weight,
                  std::vector<FrameKey> &frames) const;
    bool asTickEvent(std::uint64_t &tick_id, double &mspt) const;
    bool asStallBegin(std::uint64_t &detected_ns, std::uint64_t &last_tick_ns) const;
    bool asStallEnd(std::uint64_t &detected_ns, std::uint64_t &recovered_ns) const;
    bool asCleanEnd(std::uint64_t &timestamp_ns) const;
    bool asSessionConfig(SessionConfig &config) const;
};

// Metadata snapshot loaded from the sidecar file.  Present and valid only when
// the writer flushed a snapshot before pruning early segments.
struct MetadataSnapshot {
    bool valid = false;
    std::uint64_t session_id = 0;
    SessionConfig session_config;
    std::vector<std::pair<std::uint32_t, std::string>> modules;
    std::vector<SnapshotThreadDef> threads;
};

// Result of reading a journal session.
struct JournalReadResult {
    bool valid = false;  // at least the file header was parsed
    std::uint64_t session_id = 0;
    std::uint64_t created_ns = 0;
    bool has_clean_end = false;
    bool head_truncated = false;  // earliest retained segment is not segment 0
    bool duplicate_sequences = false;
    std::uint32_t first_segment_number = 0;
    SessionConfig session_config;
    std::optional<MetadataSnapshot> metadata_snapshot;
    std::vector<JournalRecord> records;
    std::uint64_t corrupt_records = 0;    // CRC mismatches
    std::uint64_t truncated_records = 0;  // incomplete trailing records
};

// Reads and validates recovery journal segments. Truncated records are dropped;
// CRC mismatches terminate the current segment (indicates corruption or interrupted write).
class JournalReader {
public:
    // Reads all segments in the given directory, ordered by segment number.
    static JournalReadResult readSession(const std::filesystem::path &directory);

    // Reads a single segment file.  Exposed for testing.
    static bool readSegment(const std::filesystem::path &path, JournalReadResult &result,
                            std::optional<std::uint32_t> expected_segment = std::nullopt);

    // Loads and validates the metadata snapshot sidecar.  Returns a snapshot
    // with valid=false if the file is absent, malformed, or fails CRC/session
    // validation.  Never throws.
    static MetadataSnapshot readMetadataSnapshot(const std::filesystem::path &directory) noexcept;

private:
    static MetadataSnapshot readMetadataSnapshotImpl(const std::filesystem::path &directory);
};

}  // namespace spark

#endif  // SPARK_CORE_RECOVERY_JOURNAL_READER_H
