#ifndef SPARK_CORE_RECOVERY_RECOVERY_WRITER_H
#define SPARK_CORE_RECOVERY_RECOVERY_WRITER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <moodycamel/concurrentqueue.h>

#include "core/recovery/journal_format.h"
#include "native/sampler/recovery_sink.h"

namespace spark {

// Crash-safe recovery journal writer.  Runs a dedicated thread that drains
// a bounded lock-free queue and appends serialized records to segmented
// journal files.  All file I/O happens on the writer thread; producers
// (aggregator, watchdog, main thread) only enqueue lightweight serialized
// records.
//
// Implements RecoverySink for use by the Sampler's aggregator thread.
// Additional methods (stall, clean-end, flush) are called from the
// watchdog or main thread.
class RecoveryWriter : public RecoverySink {
public:
    struct Config {
        std::filesystem::path directory;
        std::uint64_t session_id = 0;
        std::size_t max_segment_bytes = 16 * 1024 * 1024;  // 16 MiB
        std::size_t max_total_bytes = 128 * 1024 * 1024;   // 128 MiB
        int flush_interval_ms = 1000;   // batch flush
        int sync_interval_ms = 5000;    // durable sync
        std::size_t queue_capacity = 65536;
    };

    explicit RecoveryWriter(Config config);
    ~RecoveryWriter();

    RecoveryWriter(const RecoveryWriter &) = delete;
    RecoveryWriter &operator=(const RecoveryWriter &) = delete;

    // Spawns the writer thread.  Returns false if the journal file cannot
    // be created (I/O error); in that case the writer stays disabled and
    // all enqueues become no-ops.
    bool start();

    // Signals the thread to stop, flushes remaining records, closes files,
    // and joins.  Safe to call once.
    void stop();

    bool enabled() const { return enabled_.load(std::memory_order_acquire); }
    std::uint64_t droppedRecords() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t writtenRecords() const { return written_.load(std::memory_order_relaxed); }

    // --- RecoverySink (called from the aggregator thread) ---
    void journalModuleDef(std::uint32_t module_id, std::string_view path) override;
    void journalThreadDef(std::uint64_t thread_id, std::uint64_t os_thread_id,
                          std::string_view name) override;
    void journalSample(const Sample &sample) override;
    void journalTickEvent(std::uint64_t tick_id, double mspt) override;

    // --- Extended API (called from watchdog / main thread) ---
    void journalStallBegin(std::uint64_t detected_ns, std::uint64_t last_tick_ns);
    void journalStallEnd(std::uint64_t detected_ns, std::uint64_t recovered_ns);
    void journalCleanEnd();
    void journalSessionConfig(
        std::uint32_t interval_us, std::int32_t only_ticks_over_ms,
        bool all_threads, bool regex_threads, bool ignore_sleeping,
        std::uint8_t thread_grouper, std::uint8_t profile_type,
        std::string_view creator_name,
        bool creator_is_player, std::string_view comment,
        const std::vector<std::string> &thread_patterns);

    // Requests an immediate durable flush (sync to disk).
    void requestFlush();

private:
    void enqueue(RecordType type, const JournalBuffer &payload);
    void writerLoop();
    bool openSegment(std::uint32_t segment_number);
    void closeSegment();
    void rotateIfNeeded();
    bool syncFile();

    Config config_;

    std::atomic<bool> running_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<std::uint32_t> sequence_{0};
    std::atomic<std::size_t> queue_size_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> written_{0};

    moodycamel::ConcurrentQueue<std::vector<std::uint8_t>> queue_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> flush_requested_{false};

    // Writer-thread state.
    std::filesystem::path segment_path_;
    std::FILE *file_ = nullptr;
    std::uint32_t segment_number_ = 0;
    std::size_t segment_bytes_ = 0;
    std::size_t total_bytes_ = 0;
    std::chrono::steady_clock::time_point last_sync_;
};

}  // namespace spark

#endif  // SPARK_CORE_RECOVERY_RECOVERY_WRITER_H
