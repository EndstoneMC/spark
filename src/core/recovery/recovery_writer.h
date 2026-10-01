#ifndef SPARK_CORE_RECOVERY_RECOVERY_WRITER_H
#define SPARK_CORE_RECOVERY_RECOVERY_WRITER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <moodycamel/concurrentqueue.h>

#include "core/recovery/journal_format.h"
#include "native/sampler/recovery_sink.h"

namespace spark {

struct RecoveryWriterQueueTestAccess;

// Crash-safe recovery journal writer. A dedicated thread drains a bounded lock-free queue;
// all journal file lifecycle I/O belongs to that worker. Producers only enqueue serialized records.
class RecoveryWriter : public RecoverySink {
public:
    enum class IoOperation {
        Write,
        Sync,
        Close,
        Rename,
        Flush,
        PartialWrite,
        PartialWriteComplete,
        WriteComplete,
        FlushComplete,
        SyncComplete,
        CloseComplete,
        RenameComplete,
        Reopen,
        FlushRequestBeforeLock,
        WaitBeforePark,
        DrainComplete,
        SnapshotOpen,
        DegradationBeforeLock
    };

    struct Config {
        std::filesystem::path directory;
        std::uint64_t session_id = 0;
        std::size_t max_segment_bytes = 16 * 1024 * 1024;  // 16 MiB
        std::size_t max_total_bytes = 128 * 1024 * 1024;   // 128 MiB
        int flush_interval_ms = 1000;                      // batch flush
        int sync_interval_ms = 5000;                       // durable sync
        int shutdown_timeout_ms = 2000;                    // bounded caller wait
        std::size_t queue_capacity = 65536;
        // Test seam for deterministic I/O blocking/failure injection. Empty in production.
        std::function<bool(IoOperation)> io_hook;
    };

    explicit RecoveryWriter(Config config);
    ~RecoveryWriter();

    RecoveryWriter(const RecoveryWriter &) = delete;
    RecoveryWriter &operator=(const RecoveryWriter &) = delete;

    // Spawns the writer thread. Returns false if the journal cannot be created;
    // in that case the writer stays disabled and enqueues become no-ops.
    bool start();

    // Closes admission immediately. stop() waits only for the configured budget.
    // A timeout keeps the joinable worker and all state owned by this object; tryReap()
    // joins it later after workerExited() becomes true. The worker owns final drain/sync/close.
    void requestStop() noexcept;
    bool stop();
    bool stop(std::chrono::milliseconds timeout);
    bool tryReap();

    bool enabled() const { return enabled_.load(std::memory_order_acquire); }
    bool stopRequested() const { return stop_requested_.load(std::memory_order_acquire); }
    bool workerExited() const { return worker_exited_.load(std::memory_order_acquire); }
    std::uint64_t droppedRecords() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t writtenRecords() const { return written_.load(std::memory_order_relaxed); }
    // Sticky first journal I/O degradation cause: write_failed, sync_failed, rotate_failed,
    // prune_stalled or snapshot_failed.
    bool journalDegraded() const { return degradation_reported_.load(std::memory_order_acquire); }
    std::string journalDegradationReason();

    // --- RecoverySink (called from the aggregator thread) ---
    void journalModuleDef(std::uint32_t module_id, std::string_view path) override;
    void journalThreadDef(std::uint64_t thread_id, std::uint64_t os_thread_id, std::string_view name) override;
    void journalSample(const Sample &sample) override;
    void journalTickEvent(std::uint64_t tick_id, double mspt) override;

    // --- Extended API (called from watchdog / main thread) ---
    void journalStallBegin(std::uint64_t detected_ns, std::uint64_t last_tick_ns);
    void journalStallEnd(std::uint64_t detected_ns, std::uint64_t recovered_ns);
    void journalCleanEnd();
    void journalSessionConfig(std::uint32_t interval_us, std::int32_t only_ticks_over_ms, bool all_threads,
                              bool regex_threads, bool ignore_sleeping, std::uint8_t thread_grouper,
                              std::uint8_t profile_type, bool live_only, std::string_view creator_name,
                              bool creator_is_player, std::string_view comment,
                              const std::vector<std::string> &thread_patterns, std::int32_t window_adjustment_ms,
                              std::string_view creator_unique_id = {});

    // Requests an immediate durable flush.
    void requestFlush();

private:
    friend struct RecoveryWriterQueueTestAccess;

    void enqueue(RecordType type, const JournalBuffer &payload);
    void writerLoop();
    bool openSegment(std::uint32_t segment_number);
    bool closeSegment();
    void rotateIfNeeded();
    bool syncFile();
    bool writeMetadataSnapshot();
    void cacheModuleDef(std::uint32_t module_id, std::string_view path);
    void cacheThreadDef(std::uint64_t thread_id, std::uint64_t os_thread_id, std::string_view name);
    bool allowIo(IoOperation operation) const noexcept;
    bool writeFile(std::FILE *file, const void *data, std::size_t size);
    bool syncFile(std::FILE *file);
    bool closeFile(std::FILE *file);
    bool renameFile(const std::filesystem::path &from, const std::filesystem::path &to, std::error_code &ec);
    void producerDone() noexcept;
    void reportJournalDegradation(std::string_view cause) noexcept;
    void markWorkerExited() noexcept;

    Config config_;

    std::atomic<bool> running_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> accepting_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> worker_exited_{true};
    std::atomic<std::size_t> active_producers_{0};
    std::atomic<std::uint32_t> sequence_{0};
    std::atomic<std::size_t> queue_size_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> written_{0};
    std::atomic<bool> degradation_reported_{false};

    moodycamel::ConcurrentQueue<std::vector<std::uint8_t>> queue_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> flush_requested_{false};
    std::mutex exit_mutex_;
    std::condition_variable exit_cv_;
    std::mutex reap_mutex_;
    std::mutex degradation_mutex_;
    std::string degradation_reason_;

    // Writer-thread state.
    std::filesystem::path segment_path_;
    std::FILE *file_ = nullptr;
    std::uint32_t segment_number_ = 0;
    std::uint32_t first_retained_segment_ = 0;
    std::map<std::uint32_t, std::size_t> segment_sizes_;
    std::size_t segment_bytes_ = 0;
    std::size_t total_bytes_ = 0;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point last_sync_;
    int snapshot_failures_ = 0;

    // Metadata cache (protected by metadata_mutex_). Updated by producers when they
    // enqueue metadata records; read by the worker when snapshotting before pruning.
    std::mutex metadata_mutex_;
    std::vector<std::uint8_t> cached_session_config_;
    std::map<std::uint32_t, std::string> cached_modules_;
    std::map<std::uint64_t, SnapshotThreadDef> cached_threads_;
};

}  // namespace spark

#endif  // SPARK_CORE_RECOVERY_RECOVERY_WRITER_H
