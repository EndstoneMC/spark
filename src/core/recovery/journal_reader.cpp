#include "core/recovery/journal_reader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include <zlib.h>

namespace spark {

namespace {

class ByteCursor {
public:
    ByteCursor(const std::uint8_t *data, std::size_t size)
        : data_(data), size_(size), pos_(0) {}

    bool eof() const { return pos_ >= size_; }
    std::size_t remaining() const { return pos_ < size_ ? size_ - pos_ : 0; }

    bool u8(std::uint8_t &out)
    {
        if (remaining() < 1) return false;
        out = data_[pos_++];
        return true;
    }
    bool u16(std::uint16_t &out) { return read(&out, 2); }
    bool u32(std::uint32_t &out) { return read(&out, 4); }
    bool u64(std::uint64_t &out) { return read(&out, 8); }
    bool i32(std::int32_t &out) { return read(&out, 4); }
    bool f64(double &out) { return read(&out, 8); }
    bool bytes(void *out, std::size_t n)
    {
        if (remaining() < n) return false;
        std::memcpy(out, data_ + pos_, n);
        pos_ += n;
        return true;
    }
    bool stringView(std::string_view &out, std::size_t n)
    {
        if (remaining() < n) return false;
        out = std::string_view(reinterpret_cast<const char *>(data_ + pos_), n);
        pos_ += n;
        return true;
    }

private:
    bool read(void *out, std::size_t n)
    {
        if (remaining() < n) return false;
        std::memcpy(out, data_ + pos_, n);
        pos_ += n;
        return true;
    }
    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t pos_;
};

}  // namespace

// --- JournalRecord payload accessors ---

bool JournalRecord::asModuleDef(std::uint32_t &module_id, std::string &path) const
{
    ByteCursor c(payload.data(), payload.size());
    if (!c.u32(module_id)) return false;
    std::uint16_t len;
    if (!c.u16(len)) return false;
    std::string_view sv;
    if (!c.stringView(sv, len)) return false;
    path.assign(sv.data(), sv.size());
    return true;
}

bool JournalRecord::asThreadDef(std::uint64_t &thread_id, std::uint64_t &os_thread_id,
                                std::string &name) const
{
    ByteCursor c(payload.data(), payload.size());
    if (!c.u64(thread_id)) return false;
    if (!c.u64(os_thread_id)) return false;
    std::uint16_t len;
    if (!c.u16(len)) return false;
    std::string_view sv;
    if (!c.stringView(sv, len)) return false;
    name.assign(sv.data(), sv.size());
    return true;
}

bool JournalRecord::asSample(std::uint64_t &thread_id, std::uint64_t &tick_id,
                             std::int32_t &window, std::uint64_t &weight,
                             std::vector<FrameKey> &frames) const
{
    ByteCursor c(payload.data(), payload.size());
    if (!c.u64(thread_id)) return false;
    if (!c.u64(tick_id)) return false;
    if (!c.i32(window)) return false;
    if (!c.u64(weight)) return false;
    std::uint16_t frame_count;
    if (!c.u16(frame_count)) return false;
    frames.clear();
    frames.reserve(frame_count);
    for (std::uint16_t i = 0; i < frame_count; ++i) {
        FrameKey frame;
        if (!c.u32(frame.module)) return false;
        if (!c.u64(frame.rva)) return false;
        frame.raw_address = 0;  // not stored in journal; reconstructed from module base
        frames.push_back(frame);
    }
    return true;
}

bool JournalRecord::asTickEvent(std::uint64_t &tick_id, double &mspt) const
{
    ByteCursor c(payload.data(), payload.size());
    return c.u64(tick_id) && c.f64(mspt);
}

bool JournalRecord::asStallBegin(std::uint64_t &detected_ns, std::uint64_t &last_tick_ns) const
{
    ByteCursor c(payload.data(), payload.size());
    return c.u64(detected_ns) && c.u64(last_tick_ns);
}

bool JournalRecord::asStallEnd(std::uint64_t &detected_ns, std::uint64_t &recovered_ns) const
{
    ByteCursor c(payload.data(), payload.size());
    return c.u64(detected_ns) && c.u64(recovered_ns);
}

bool JournalRecord::asCleanEnd(std::uint64_t &timestamp_ns) const
{
    ByteCursor c(payload.data(), payload.size());
    return c.u64(timestamp_ns);
}

bool JournalRecord::asSessionConfig(SessionConfig &config) const
{
    ByteCursor c(payload.data(), payload.size());
    if (!c.u32(config.interval_us)) return false;
    if (!c.i32(config.only_ticks_over_ms)) return false;
    std::uint8_t b;
    if (!c.u8(b)) return false;
    config.all_threads = b != 0;
    if (!c.u8(b)) return false;
    config.regex_threads = b != 0;
    if (!c.u8(b)) return false;
    config.ignore_sleeping = b != 0;
    if (!c.u8(config.thread_grouper)) return false;
    std::uint16_t len;
    if (!c.u16(len)) return false;
    std::string_view sv;
    if (!c.stringView(sv, len)) return false;
    config.creator_name.assign(sv.data(), sv.size());
    if (!c.u8(b)) return false;
    config.creator_is_player = b != 0;
    if (!c.u16(len)) return false;
    if (!c.stringView(sv, len)) return false;
    config.comment.assign(sv.data(), sv.size());
    std::uint16_t count;
    if (!c.u16(count)) return false;
    config.thread_patterns.clear();
    config.thread_patterns.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        if (!c.u16(len)) return false;
        if (!c.stringView(sv, len)) return false;
        config.thread_patterns.emplace_back(sv.data(), sv.size());
    }
    config.present = true;
    return true;
}

// --- JournalReader ---

bool JournalReader::readSegment(const std::filesystem::path &path, JournalReadResult &result)
{
    std::FILE *f = std::fopen(path.string().c_str(), "rb");
    if (!f) return false;

    // Read entire file into memory.
    std::fseek(f, 0, SEEK_END);
    long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        std::fclose(f);
        return false;
    }

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(file_size));
    std::size_t read = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (read != buf.size()) return false;

    ByteCursor c(buf.data(), buf.size());

    // File header.
    std::uint8_t magic[8];
    if (!c.bytes(magic, 8)) return false;
    if (std::memcmp(magic, kJournalMagic, 8) != 0) return false;
    std::uint16_t version;
    if (!c.u16(version)) return false;
    if (version != kJournalVersion) return false;
    std::uint16_t reserved;
    if (!c.u16(reserved)) return false;
    std::uint64_t session_id;
    if (!c.u64(session_id)) return false;
    std::uint64_t created_ns;
    if (!c.u64(created_ns)) return false;
    std::uint32_t segment_number;
    if (!c.u32(segment_number)) return false;

    if (!result.valid) {
        result.session_id = session_id;
        result.created_ns = created_ns;
        result.valid = true;
    }

    // Records.
    while (!c.eof()) {
        std::uint8_t type_byte;
        if (!c.u8(type_byte)) { result.truncated_records++; break; }
        std::uint8_t rsv;
        if (!c.u8(rsv)) { result.truncated_records++; break; }
        std::uint32_t seq;
        if (!c.u32(seq)) { result.truncated_records++; break; }
        std::uint32_t payload_len;
        if (!c.u32(payload_len)) { result.truncated_records++; break; }
        if (payload_len > kMaxPayloadSize) { result.corrupt_records++; break; }
        std::uint32_t crc;
        if (!c.u32(crc)) { result.truncated_records++; break; }

        // Check payload is fully present.
        if (c.remaining() < payload_len) {
            result.truncated_records++;
            break;
        }

        const std::uint8_t *payload_ptr = buf.data() + (buf.size() - c.remaining());

        // Verify CRC.
        std::uint32_t actual_crc = static_cast<std::uint32_t>(
            crc32(0L, payload_ptr, payload_len));
        if (actual_crc != crc) {
            result.corrupt_records++;
            break;  // stop reading this segment
        }

        std::string_view payload_sv;
        if (!c.stringView(payload_sv, payload_len)) {
            result.truncated_records++;
            break;
        }

        JournalRecord rec;
        rec.type = static_cast<RecordType>(type_byte);
        rec.sequence = seq;
        rec.payload.assign(payload_sv.data(), payload_sv.data() + payload_sv.size());

        if (rec.type == RecordType::CleanEnd) {
            result.has_clean_end = true;
        }
        if (rec.type == RecordType::SessionConfig && !result.session_config.present) {
            rec.asSessionConfig(result.session_config);
        }
        result.records.push_back(std::move(rec));
    }

    return true;
}

JournalReadResult JournalReader::readSession(const std::filesystem::path &directory)
{
    JournalReadResult result;
    std::error_code ec;

    // Collect segment files sorted by segment number.
    std::set<std::pair<std::uint32_t, std::filesystem::path>> segments;
    for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto name = entry.path().filename().string();
        if (name.size() < 8 || name.substr(0, 8) != "segment-") continue;
        if (name.size() < 4 || name.substr(name.size() - 4) != ".jnl") continue;

        std::string num_str = name.substr(8, name.size() - 12);
        try {
            std::uint32_t num = static_cast<std::uint32_t>(std::stoul(num_str));
            segments.emplace(num, entry.path());
        } catch (...) {
            continue;
        }
    }

    for (const auto &[num, path] : segments) {
        readSegment(path, result);
    }

    return result;
}

}  // namespace spark
