#include "core/recovery/recovery_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace spark {

namespace {

std::uint64_t monotonicNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool syncFileImpl(std::FILE *f)
{
#if defined(_WIN32)
    return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)))) != 0;
#else
    return fdatasync(fileno(f)) == 0;
#endif
}

}  // namespace

RecoveryWriter::RecoveryWriter(Config config)
    : config_(std::move(config))
{
}

RecoveryWriter::~RecoveryWriter()
{
    stop();
}

bool RecoveryWriter::start()
{
    if (running_.exchange(true)) return true;

    std::error_code ec;
    std::filesystem::create_directories(config_.directory, ec);
    if (ec) {
        enabled_.store(false);
        running_.store(false);
        return false;
    }

    if (!openSegment(0)) {
        enabled_.store(false);
        running_.store(false);
        return false;
    }

    enabled_.store(true);
    last_sync_ = std::chrono::steady_clock::now();
    thread_ = std::thread([this]() { writerLoop(); });
    return true;
}

void RecoveryWriter::stop()
{
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    // Final drain + flush.
    if (file_) {
        syncFile();
        closeSegment();
    }
    enabled_.store(false);
}

void RecoveryWriter::enqueue(RecordType type, const JournalBuffer &payload)
{
    if (!enabled_.load(std::memory_order_acquire)) return;

    const std::size_t approx = queue_size_.load(std::memory_order_relaxed);
    if (approx >= config_.queue_capacity) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const std::uint32_t seq = sequence_.fetch_add(1, std::memory_order_relaxed);
    auto record = serializeRecord(type, seq, payload);
    queue_.enqueue(std::move(record));
    queue_size_.fetch_add(1, std::memory_order_relaxed);
}

void RecoveryWriter::journalModuleDef(std::uint32_t module_id, std::string_view path)
{
    enqueue(RecordType::ModuleDef, buildModuleDefPayload(module_id, path));
}

void RecoveryWriter::journalThreadDef(std::uint64_t thread_id, std::uint64_t os_thread_id,
                                      std::string_view name)
{
    enqueue(RecordType::ThreadDef, buildThreadDefPayload(thread_id, os_thread_id, name));
}

void RecoveryWriter::journalSample(const Sample &sample)
{
    enqueue(RecordType::Sample, buildSamplePayload(sample));
}

void RecoveryWriter::journalTickEvent(std::uint64_t tick_id, double mspt)
{
    enqueue(RecordType::TickEvent, buildTickEventPayload(tick_id, mspt));
}

void RecoveryWriter::journalStallBegin(std::uint64_t detected_ns, std::uint64_t last_tick_ns)
{
    enqueue(RecordType::StallBegin, buildStallBeginPayload(detected_ns, last_tick_ns));
}

void RecoveryWriter::journalStallEnd(std::uint64_t detected_ns, std::uint64_t recovered_ns)
{
    enqueue(RecordType::StallEnd, buildStallEndPayload(detected_ns, recovered_ns));
}

void RecoveryWriter::journalCleanEnd()
{
    enqueue(RecordType::CleanEnd, buildCleanEndPayload(monotonicNowNs()));
    requestFlush();
}

void RecoveryWriter::journalSessionConfig(
    std::uint32_t interval_us, std::int32_t only_ticks_over_ms,
    bool all_threads, bool regex_threads, bool ignore_sleeping,
    std::uint8_t thread_grouper, std::string_view creator_name,
    bool creator_is_player, std::string_view comment,
    const std::vector<std::string> &thread_patterns)
{
    enqueue(RecordType::SessionConfig,
            buildSessionConfigPayload(interval_us, only_ticks_over_ms,
                                      all_threads, regex_threads, ignore_sleeping,
                                      thread_grouper, creator_name,
                                      creator_is_player, comment, thread_patterns));
}

void RecoveryWriter::requestFlush()
{
    flush_requested_.store(true, std::memory_order_release);
    cv_.notify_one();
}

bool RecoveryWriter::openSegment(std::uint32_t segment_number)
{
    segment_path_ = config_.directory / ("segment-" + std::to_string(segment_number) + ".jnl");
    file_ = std::fopen(segment_path_.string().c_str(), "wb");
    if (!file_) {
        return false;
    }

    auto header = serializeFileHeader(config_.session_id, monotonicNowNs(), segment_number);
    if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }

    segment_number_ = segment_number;
    segment_bytes_ = header.size();
    total_bytes_ += header.size();
    return true;
}

void RecoveryWriter::closeSegment()
{
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool RecoveryWriter::syncFile()
{
    if (!file_) return false;
    if (!syncFileImpl(file_)) {
        // Disable recovery for the rest of the session.
        enabled_.store(false, std::memory_order_release);
        return false;
    }
    last_sync_ = std::chrono::steady_clock::now();
    return true;
}

void RecoveryWriter::rotateIfNeeded()
{
    if (total_bytes_ <= config_.max_total_bytes) return;

    // Delete oldest segments until under limit.  Segments are numbered
    // sequentially from 0; the lowest number is the oldest.
    for (std::uint32_t i = 0; i < segment_number_ && total_bytes_ > config_.max_total_bytes; ++i) {
        auto path = config_.directory / ("segment-" + std::to_string(i) + ".jnl");
        std::error_code ec;
        auto size = std::filesystem::file_size(path, ec);
        if (!ec && std::filesystem::remove(path, ec)) {
            total_bytes_ -= (size > 0 ? static_cast<std::size_t>(size) : 0);
        }
    }
}

void RecoveryWriter::writerLoop()
{
    const auto flush_interval = std::chrono::milliseconds(config_.flush_interval_ms);
    const auto sync_interval = std::chrono::milliseconds(config_.sync_interval_ms);
    std::vector<std::uint8_t> record;

    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, flush_interval, [this] {
                return !running_.load() || flush_requested_.load();
            });
        }
        flush_requested_.store(false, std::memory_order_release);

        if (!enabled_.load(std::memory_order_acquire)) break;

        bool wrote_any = false;
        while (queue_.try_dequeue(record)) {
            queue_size_.fetch_sub(1, std::memory_order_relaxed);
            if (file_) {
                if (std::fwrite(record.data(), 1, record.size(), file_) != record.size()) {
                    enabled_.store(false, std::memory_order_release);
                    break;
                }
                segment_bytes_ += record.size();
                total_bytes_ += record.size();
                written_.fetch_add(1, std::memory_order_relaxed);
                wrote_any = true;

                // Segment rotation.
                if (segment_bytes_ >= config_.max_segment_bytes) {
                    syncFile();
                    closeSegment();
                    if (!openSegment(segment_number_ + 1)) {
                        enabled_.store(false, std::memory_order_release);
                        break;
                    }
                    rotateIfNeeded();
                }
            }
        }

        // Periodic sync.
        if (wrote_any) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_sync_ >= sync_interval) {
                syncFile();
            }
        }
    }

    // Final drain.
    if (enabled_.load(std::memory_order_acquire)) {
        while (queue_.try_dequeue(record)) {
            queue_size_.fetch_sub(1, std::memory_order_relaxed);
            if (file_) {
                if (std::fwrite(record.data(), 1, record.size(), file_) != record.size()) {
                    enabled_.store(false, std::memory_order_release);
                    break;
                }
                segment_bytes_ += record.size();
                total_bytes_ += record.size();
                written_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        syncFile();
    }
}

}  // namespace spark
