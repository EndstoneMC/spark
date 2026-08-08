#ifndef SPARK_CORE_RECOVERY_JOURNAL_FORMAT_H
#define SPARK_CORE_RECOVERY_JOURNAL_FORMAT_H

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

#include "native/sampler/types.h"

namespace spark {

// Journal format constants.
inline constexpr char kJournalMagic[8] = {'S', 'P', 'R', 'K', 'J', 'N', 'R', 'L'};
inline constexpr std::uint16_t kJournalVersion = 2;

// File header size: 8 (magic) + 2 (version) + 2 (reserved) + 8 (session_id) + 8 (created_ns) + 4 (segment) = 32
inline constexpr std::size_t kFileHeaderSize = 32;

// Record header size: 1 (type) + 1 (reserved) + 4 (sequence) + 4 (payload_len) + 4 (crc32) = 14
inline constexpr std::size_t kRecordHeaderSize = 14;

enum class RecordType : std::uint8_t {
    ModuleDef = 1,
    ThreadDef = 2,
    Sample    = 3,
    TickEvent = 4,
    StallBegin = 5,
    StallEnd   = 6,
    CleanEnd   = 7,
    SessionConfig = 8,
};

// Maximum payload size guard (prevents a corrupt length field from causing
// an oversized allocation during read).
inline constexpr std::uint32_t kMaxPayloadSize = 1024 * 1024;  // 1 MiB

// Append-only byte buffer with little-endian writes.
class JournalBuffer {
public:
    void clear() { buf_.clear(); }
    const std::uint8_t *data() const { return buf_.data(); }
    std::size_t size() const { return buf_.size(); }
    std::vector<std::uint8_t> take() { return std::move(buf_); }

    void u8(std::uint8_t v) { buf_.push_back(v); }
    void u16(std::uint16_t v) { append(&v, 2); }
    void u32(std::uint32_t v) { append(&v, 4); }
    void u64(std::uint64_t v) { append(&v, 8); }
    void i32(std::int32_t v) { append(&v, 4); }
    void f64(double v) { append(&v, 8); }
    void bytes(const void *p, std::size_t n) { append(p, n); }
    void str(std::string_view s) { u16(static_cast<std::uint16_t>(s.size())); append(s.data(), s.size()); }

private:
    void append(const void *p, std::size_t n)
    {
        const auto *b = static_cast<const std::uint8_t *>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<std::uint8_t> buf_;
};

// Serialize a complete record (header + payload + CRC) into a buffer.
// Returns the serialized bytes.
inline std::vector<std::uint8_t> serializeRecord(RecordType type, std::uint32_t sequence,
                                                  const JournalBuffer &payload)
{
    const std::uint32_t payload_len = static_cast<std::uint32_t>(payload.size());
    const std::uint32_t crc = static_cast<std::uint32_t>(
        crc32(0L, payload.data(), payload_len));

    JournalBuffer rec;
    rec.u8(static_cast<std::uint8_t>(type));
    rec.u8(0);  // reserved
    rec.u32(sequence);
    rec.u32(payload_len);
    rec.u32(crc);
    rec.bytes(payload.data(), payload_len);
    return rec.take();
}

// Serialize the file header.
inline std::vector<std::uint8_t> serializeFileHeader(std::uint64_t session_id,
                                                      std::uint64_t created_ns,
                                                      std::uint32_t segment_number)
{
    JournalBuffer h;
    h.bytes(kJournalMagic, 8);
    h.u16(kJournalVersion);
    h.u16(0);  // reserved
    h.u64(session_id);
    h.u64(created_ns);
    h.u32(segment_number);
    return h.take();
}

// Payload builders.  Each builds the payload portion (without the record
// header); serializeRecord() wraps it with type/sequence/length/CRC.

inline JournalBuffer buildModuleDefPayload(std::uint32_t module_id, std::string_view path)
{
    JournalBuffer p;
    p.u32(module_id);
    p.str(path);
    return p;
}

inline JournalBuffer buildThreadDefPayload(std::uint64_t thread_id, std::uint64_t os_thread_id,
                                           std::string_view name)
{
    JournalBuffer p;
    p.u64(thread_id);
    p.u64(os_thread_id);
    p.str(name);
    return p;
}

inline JournalBuffer buildSamplePayload(const Sample &sample)
{
    JournalBuffer p;
    p.u64(sample.thread_id);
    p.u64(sample.tick_id);
    p.i32(sample.window);
    p.u64(sample.weight);
    const std::uint16_t frame_count = static_cast<std::uint16_t>(sample.frames.size());
    p.u16(frame_count);
    for (const auto &frame : sample.frames) {
        p.u32(frame.module);
        p.u64(frame.rva);
    }
    return p;
}

inline JournalBuffer buildTickEventPayload(std::uint64_t tick_id, double mspt)
{
    JournalBuffer p;
    p.u64(tick_id);
    p.f64(mspt);
    return p;
}

inline JournalBuffer buildStallBeginPayload(std::uint64_t detected_ns, std::uint64_t last_tick_ns)
{
    JournalBuffer p;
    p.u64(detected_ns);
    p.u64(last_tick_ns);
    return p;
}

inline JournalBuffer buildStallEndPayload(std::uint64_t detected_ns, std::uint64_t recovered_ns)
{
    JournalBuffer p;
    p.u64(detected_ns);
    p.u64(recovered_ns);
    return p;
}

inline JournalBuffer buildCleanEndPayload(std::uint64_t timestamp_ns)
{
    JournalBuffer p;
    p.u64(timestamp_ns);
    return p;
}

inline JournalBuffer buildSessionConfigPayload(
    std::uint32_t interval_us, std::int32_t only_ticks_over_ms,
    bool all_threads, bool regex_threads, bool ignore_sleeping,
    std::uint8_t thread_grouper, std::uint8_t profile_type, bool live_only,
    std::string_view creator_name,
    bool creator_is_player, std::string_view comment,
    const std::vector<std::string> &thread_patterns)
{
    JournalBuffer p;
    p.u32(interval_us);
    p.i32(only_ticks_over_ms);
    p.u8(all_threads ? 1 : 0);
    p.u8(regex_threads ? 1 : 0);
    p.u8(ignore_sleeping ? 1 : 0);
    p.u8(thread_grouper);
    p.u8(profile_type);
    p.u8(live_only ? 1 : 0);
    p.str(creator_name);
    p.u8(creator_is_player ? 1 : 0);
    p.str(comment);
    p.u16(static_cast<std::uint16_t>(thread_patterns.size()));
    for (const auto &pat : thread_patterns) {
        p.str(pat);
    }
    return p;
}

}  // namespace spark

#endif  // SPARK_CORE_RECOVERY_JOURNAL_FORMAT_H
