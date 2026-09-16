#include "native/alloc/allocation_sampler.h"

#if !defined(__linux__) || !defined(__x86_64__)
#error "allocation_sampler_linux.cpp requires Linux x86-64"
#endif

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cpptrace/cpptrace.hpp>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "native/alloc/allocation_profile_aggregation.h"
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
#include "native/alloc/allocation_diagnostics_test_access.h"
#include "native/alloc/allocation_lifecycle_test_access.h"
#endif
#include "native/alloc/allocation_quiescence.h"
#include "native/alloc/bounded_event_queue.h"
#include "native/alloc/byte_sampler.h"
#include "native/alloc/elf_import_hooks.h"
#include "native/alloc/linux_allocation_gateway_client.h"
#include "native/alloc/linux_owned_thread.h"
#include "native/alloc/stable_shard_snapshot.h"
#include "native/sampler/thread_info.h"
#include "profiling_window.h"

namespace spark {
namespace {

constexpr std::size_t KStackDepth = 48;
constexpr std::size_t KEventCapacity = 16384;
constexpr std::size_t KLiveIndexCapacity = KEventCapacity * 2;
constexpr std::size_t KLiveIndexShards = 64;
constexpr std::size_t KLiveIndexShardCapacity = KLiveIndexCapacity / KLiveIndexShards;
constexpr std::size_t KLivePresenceBuckets = KLiveIndexCapacity;
constexpr std::size_t KHookCallShards = 1024;
constexpr std::size_t KLiveLockAttempts = 64;
constexpr std::size_t KMaxSampledThreads = 256;
constexpr std::size_t KMaxAllocationModules = 512;
constexpr std::size_t KMaxProfileNodes = 131072;
constexpr std::size_t KMaxPendingSamples = 32768;
constexpr std::size_t KMaxTickDecisions = 100000;
constexpr std::size_t KTickEventCapacity = 4096;

#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
struct AllocationThreadCreationFailureForTesting {};

void yieldLifecycleTest() noexcept
{
    (void)::sched_yield();
}

void diagnosticsTestFrameAnchor() noexcept {}

bool waitFixtureWorkerGate(test::AllocationFixtureWorkerGate &gate) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(gate.timeout_ms);
    while (!gate.release.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            gate.timed_out.store(true, std::memory_order_release);
            break;
        }
        yieldLifecycleTest();
    }
    try {
        std::lock_guard lock(gate.seed_mutex);
    }
    catch (...) {
        gate.timed_out.store(true, std::memory_order_release);
        return false;
    }
    return !gate.timed_out.load(std::memory_order_acquire);
}
#endif

// cpptrace::safe_generate_raw_trace adds one frame to the requested skip internally.
// Skip only recordAllocation -> handleMalloc -> hookMalloc here so the real allocating caller is retained.
constexpr std::size_t KFramesToSkip = 3;

void *tombstonePointer() noexcept
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(1));
}

std::uint64_t monotonicMs() noexcept
{
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000 + static_cast<std::uint64_t>(value.tv_nsec) / 1000000;
}

std::uint64_t monotonicNs() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::uint64_t saturatingMultiply(std::uint64_t a, std::uint64_t b) noexcept
{
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    if (a == 0 || b == 0) {
        return 0;
    }
    return a > maximum / b ? maximum : a * b;
}

bool checkedMultiply(std::size_t a, std::size_t b, std::uint64_t &out) noexcept
{
    if (a != 0 && b > (std::numeric_limits<std::size_t>::max)() / a) {
        out = 0;
        return false;
    }
    out = static_cast<std::uint64_t>(a * b);
    return true;
}

}  // namespace

struct AllocationSampler::Impl {
    using MallocFn = void *(*)(std::size_t);
    using CallocFn = void *(*)(std::size_t, std::size_t);
    using ReallocFn = void *(*)(void *, std::size_t);
    using FreeFn = void (*)(void *);
    using ReallocArrayFn = void *(*)(void *, std::size_t, std::size_t);
    using AlignedAllocFn = void *(*)(std::size_t, std::size_t);
    using PosixMemalignFn = int (*)(void **, std::size_t, std::size_t);

    struct AllocationEvent {
        std::uint64_t weight_bytes = 0;
        std::uint64_t tick_id = 0;
        std::uint64_t thread_id = 0;
        std::uint64_t os_thread_id = 0;
        std::int32_t window = 0;
        std::uint16_t depth = 0;
        bool thread_observation = false;
        cpptrace::frame_ptr frames[KStackDepth]{};
    };

    struct TickEvent {
        std::uint64_t tick_id = 0;
        double mspt_ms = 0.0;
    };

    struct LiveAllocation {
        LiveAllocation *next = nullptr;
        void *pointer = nullptr;
        std::uint64_t allocation_id = 0;
        std::uint64_t weight_bytes = 0;
        std::uint64_t requested_bytes = 0;
        std::uint64_t allocated_ms = 0;
        std::uint64_t tick_id = 0;
        std::uint64_t thread_id = 0;
        std::uint64_t os_thread_id = 0;
        std::int32_t window = 0;
        std::uint16_t depth = 0;
        cpptrace::frame_ptr frames[KStackDepth]{};
    };

    struct LiveIndexEntry {
        void *pointer = nullptr;
        std::uint64_t allocation_id = 0;
        LiveAllocation *allocation = nullptr;
    };

    struct EventQueue {
        struct Cell {
            std::atomic<std::size_t> sequence{0};
            AllocationEvent event;
        };

        Cell *storage = nullptr;
        std::atomic<std::size_t> producer{0};
        std::atomic<std::size_t> consumer{0};
        std::atomic<std::uint64_t> size{0};
        std::atomic<std::uint64_t> high_water{0};

        bool allocate(std::string &error)
        {
            const std::size_t bytes = sizeof(Cell) * KEventCapacity;
            void *memory = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (memory == MAP_FAILED) {
                storage = nullptr;
                error = "mmap for Linux allocation sample buffer failed: " + std::string(std::strerror(errno));
                return false;
            }
            storage = static_cast<Cell *>(memory);
            for (std::size_t i = 0; i < KEventCapacity; ++i) {
                ::new (static_cast<void *>(&storage[i])) Cell{};
                storage[i].sequence.store(i, std::memory_order_relaxed);
            }
            producer.store(0, std::memory_order_relaxed);
            consumer.store(0, std::memory_order_relaxed);
            size.store(0, std::memory_order_relaxed);
            high_water.store(0, std::memory_order_relaxed);
            return true;
        }

        void release() noexcept
        {
            if (storage != nullptr) {
                ::munmap(storage, sizeof(Cell) * KEventCapacity);
                storage = nullptr;
            }
            producer.store(0, std::memory_order_relaxed);
            consumer.store(0, std::memory_order_relaxed);
            size.store(0, std::memory_order_relaxed);
        }

        bool enqueue(const AllocationEvent &event) noexcept
        {
            std::size_t position = producer.load(std::memory_order_relaxed);
            Cell *cell = nullptr;
            bool reserved = false;
            for (std::size_t attempt = 0; attempt < KBoundedEventQueueMaxAttempts; ++attempt) {
                cell = &storage[position & (KEventCapacity - 1)];
                const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
                const std::intptr_t difference =
                    static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);
                if (difference == 0) {
                    if (producer.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                        reserved = true;
                        break;
                    }
                }
                else if (difference < 0) {
                    return false;
                }
                else {
                    position = producer.load(std::memory_order_relaxed);
                }
            }
            if (!reserved) {
                return false;
            }
            const std::uint64_t current = size.fetch_add(1, std::memory_order_relaxed) + 1;
            std::uint64_t previous = high_water.load(std::memory_order_relaxed);
            for (std::size_t attempt = 0; attempt < KBoundedEventQueueMaxAttempts && previous < current; ++attempt) {
                if (high_water.compare_exchange_weak(previous, current, std::memory_order_relaxed)) {
                    break;
                }
            }
            cell->event = event;
            cell->sequence.store(position + 1, std::memory_order_release);
            return true;
        }

        bool dequeue(AllocationEvent &event) noexcept
        {
            std::size_t position = consumer.load(std::memory_order_relaxed);
            Cell *cell = nullptr;
            bool reserved = false;
            for (std::size_t attempt = 0; attempt < KBoundedEventQueueMaxAttempts; ++attempt) {
                cell = &storage[position & (KEventCapacity - 1)];
                const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
                const std::intptr_t difference =
                    static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position + 1);
                if (difference == 0) {
                    if (consumer.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                        reserved = true;
                        break;
                    }
                }
                else if (difference < 0) {
                    return false;
                }
                else {
                    position = consumer.load(std::memory_order_relaxed);
                }
            }
            if (!reserved) {
                return false;
            }
            event = cell->event;
            size.fetch_sub(1, std::memory_order_relaxed);
            cell->sequence.store(position + KEventCapacity, std::memory_order_release);
            return true;
        }
    };

    struct ThreadSamplingState {
        std::atomic<std::uint8_t> registry_state{0};
        std::atomic<std::uint64_t> owner_tid{0};
        ByteSamplingState bytes;
        std::uint64_t identity_generation = 0;
        std::uint64_t session_thread_id = 0;
        std::uint64_t os_thread_id = 0;
        bool inside_hook = false;
        bool tracking_suppressed = false;
        bool identity_announced = false;
    };

    inline static thread_local bool mCountOnlyInsideHook = false;

    static std::size_t currentHookShard() noexcept
    {
        auto value = reinterpret_cast<std::uintptr_t>(__builtin_thread_pointer());
        value ^= value >> 17;
        value *= 0x9e3779b97f4a7c15ULL;
        value ^= value >> 29;
        return static_cast<std::size_t>(value & (KHookCallShards - 1));
    }

    class TrackingCallGuard {
    public:
        explicit TrackingCallGuard(Impl &impl) noexcept : impl_(impl), shard_(currentHookShard())
        {
            if (!impl_.tracking.load(std::memory_order_acquire)) {
                return;
            }
            impl_.tracking_calls[shard_].fetch_add(1, std::memory_order_acq_rel);
            if (impl_.tracking.load(std::memory_order_acquire)) {
                active_ = true;
            }
            else {
                impl_.tracking_calls[shard_].fetch_sub(1, std::memory_order_release);
            }
        }
        ~TrackingCallGuard()
        {
            if (active_) {
                impl_.tracking_calls[shard_].fetch_sub(1, std::memory_order_release);
            }
        }
        explicit operator bool() const noexcept { return active_; }

    private:
        Impl &impl_;
        std::size_t shard_ = 0;
        bool active_ = false;
    };

    class RecursionGuard {
    public:
        explicit RecursionGuard(Impl &impl) noexcept
        {
            if (impl.config.count_only) {
                if (!mCountOnlyInsideHook) {
                    mCountOnlyInsideHook = true;
                    count_only_owner_ = true;
                    owner_ = true;
                }
                return;
            }
            state_ = impl.currentThreadState();
            if (state_ != nullptr && !state_->inside_hook) {
                state_->inside_hook = true;
                owner_ = true;
            }
        }

        ~RecursionGuard()
        {
            if (count_only_owner_) {
                mCountOnlyInsideHook = false;
                return;
            }
            if (owner_) {
                state_->inside_hook = false;
            }
        }

        [[nodiscard]] bool owner() const noexcept { return owner_; }

    private:
        ThreadSamplingState *state_ = nullptr;
        bool count_only_owner_ = false;
        bool owner_ = false;
    };

    static constexpr std::uint64_t KConsumersClosed = std::uint64_t{1} << 63;

    class ConsumerGuard {
    public:
        explicit ConsumerGuard(Impl &impl) noexcept : ConsumerGuard(impl.consumer_state) {}
        explicit ConsumerGuard(std::atomic<std::uint64_t> &state) noexcept : state_(state)
        {
            const auto previous = state_.fetch_add(1, std::memory_order_acq_rel);
            active_ = (previous & KConsumersClosed) == 0;
            if (!active_) {
                state_.fetch_sub(1, std::memory_order_release);
            }
        }
        ~ConsumerGuard()
        {
            if (active_) {
                state_.fetch_sub(1, std::memory_order_release);
            }
        }
        ConsumerGuard(const ConsumerGuard &) = delete;
        ConsumerGuard &operator=(const ConsumerGuard &) = delete;
        explicit operator bool() const noexcept { return active_; }

    private:
        std::atomic<std::uint64_t> &state_;
        bool active_ = false;
    };

    class TrackingSuppressionGuard {
    public:
        explicit TrackingSuppressionGuard(Impl &impl, bool snapshot = false) noexcept
            : state_(impl.currentThreadState())
        {
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
            control_ = snapshot ? impl.linux_control_for_testing : nullptr;
#else
            (void)snapshot;
#endif
            if (state_ != nullptr) {
                previous_ = state_->tracking_suppressed;
                state_->tracking_suppressed = true;
            }
        }
        ~TrackingSuppressionGuard()
        {
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
            if (control_ != nullptr && control_->snapshot_before_restore != nullptr) {
                control_->snapshot_before_restore();
            }
#endif
            if (state_ != nullptr) {
                state_->tracking_suppressed = previous_;
            }
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
            if (control_ != nullptr) {
                control_->snapshot_restored.store(true, std::memory_order_release);
            }
#endif
        }

    private:
        ThreadSamplingState *state_ = nullptr;
        bool previous_ = false;
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        test::LinuxAllocationTestControl *control_ = nullptr;
#endif
    };

    static std::atomic<Impl *> mActiveInstance;

    ElfImportHooks hooks;
    LinuxAllocationGateway gateway;
    MallocFn real_malloc = nullptr;
    CallocFn real_calloc = nullptr;
    ReallocFn real_realloc = nullptr;
    FreeFn real_free = nullptr;
    ReallocArrayFn real_reallocarray = nullptr;
    AlignedAllocFn real_aligned_alloc = nullptr;
    PosixMemalignFn real_posix_memalign = nullptr;
    std::vector<AllocationHookCapability> hook_capabilities;

    std::timed_mutex lifecycle_mutex;
    std::timed_mutex tick_mutex;
    std::timed_mutex aggregate_mutex;
    AllocationSamplerConfig config{};
    std::atomic<bool> tracking{false};
    std::atomic<std::uint64_t> consumer_state{KConsumersClosed};
    std::atomic<std::uint64_t> registry_consumers{KConsumersClosed};
    std::atomic<bool> running{false};
    std::atomic<AllocationAccountingState> accounting_state{AllocationAccountingState::NotStarted};
    std::atomic<bool> tick_admission_open{true};
    std::atomic<bool> finalize_pending{false};
    std::atomic<bool> pending_finalized{false};
    std::atomic<std::uint64_t> terminal_tick{0};
    std::atomic<std::uint64_t> terminal_ms{0};
    std::atomic<bool> terminal_captured{false};
    std::atomic<std::uint64_t> tick_calls{0};
    std::atomic<bool> aggregator_running{false};
    std::atomic<bool> aggregator_failed{false};
    std::atomic<bool> backend_cleanup_pending{false};
    std::atomic<bool> backend_shutdown_pending{false};
    std::atomic<bool> failed_start_pending{false};
    std::atomic<bool> stop_wait_timed_out{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> session_ready{false};
    std::atomic<bool> rescan_requested{false};
    std::atomic<bool> rescan_active{false};
    std::atomic<bool> hooks_installed{false};
    std::atomic<std::size_t> hook_target_count{0};
    std::atomic<std::size_t> failed_module_count{0};
    std::atomic<std::size_t> hooked_module_count{0};
    std::atomic<std::size_t> skipped_module_count{0};
    struct CapabilityMailbox {
        std::uint64_t epoch = 0;
        std::vector<AllocationHookCapability> values;
    };
    std::atomic<CapabilityMailbox *> capabilities_pending{nullptr};
    enum class BackendState {
        Idle,
        Starting,
        Active,
        Cleaning,
        Failed,
        Exited
    };
    std::atomic<BackendState> backend_state{BackendState::Idle};
    std::atomic<std::uint64_t> backend_revision{0};
    std::atomic<std::uint64_t> backend_completed_revision{0};
    std::atomic<bool> backend_success{false};
    std::atomic<std::int64_t> backend_deadline_ns{0};
    std::atomic<std::string *> backend_message{nullptr};
    AllocationSamplerConfig pending_config{};
    LinuxOwnedThread backend_thread;
    bool gateway_published = false;
    std::array<std::atomic<char>, 256> aggregator_failure{};
    std::array<std::atomic<std::uint64_t>, KHookCallShards> tracking_calls{};
    std::atomic<std::uint64_t> current_tick{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> interval_bytes{kDefaultAllocationIntervalBytes};
    std::atomic<std::uint64_t> sampling_seed{0};
    struct alignas(64) HotCounters {
        std::atomic<std::uint64_t> hook_calls{0};
        std::atomic<std::uint64_t> successful_allocation_calls{0};
        std::atomic<std::uint64_t> observed_bytes{0};
    };
    static_assert(sizeof(HotCounters) <= 64);
    std::array<HotCounters, KHookCallShards> hot_counters{};
    std::atomic<std::uint64_t> sampling_points{0};
    std::atomic<std::uint64_t> filtered_samples{0};
    std::atomic<std::uint64_t> dropped_samples{0};
    std::atomic<std::uint64_t> dropped_events{0};
    std::atomic<std::uint64_t> dropped_tick_events{0};
    std::atomic<std::uint64_t> enqueued_samples{0};
    std::atomic<std::uint64_t> next_allocation_id{1};
    std::atomic<std::uint64_t> next_session_thread_id{1};
    std::atomic<std::uint64_t> registered_threads{0};
    std::atomic<std::uint64_t> overflow_threads{0};
    std::atomic<std::uint64_t> thread_state_drops{0};
    std::atomic<std::uint64_t> freed_samples{0};
    std::atomic<std::uint64_t> freed_bytes{0};
    std::atomic<std::uint64_t> live_samples{0};
    std::atomic<std::uint64_t> live_bytes{0};
    std::atomic<std::uint64_t> peak_live_samples{0};
    std::atomic<std::uint64_t> lifetime_ms_total{0};
    std::atomic<std::uint64_t> lifetime_ms_max{0};
    std::atomic<std::uint64_t> lifecycle_dropped{0};
    std::atomic<std::uint64_t> contention_dropped{0};
    std::atomic<std::uint64_t> lifecycle_version{0};
    std::atomic<std::uint64_t> lifecycle_readers{0};
    std::atomic<std::uint64_t> lifecycle_writers{0};
    std::atomic<std::uint64_t> retained_age_ms_total{0};
    std::atomic<std::uint64_t> retained_age_ms_max{0};
    std::atomic<std::uint64_t> drain_truncated{0};
    std::atomic<std::uint64_t> drain_truncated_allocation_events{0};
    std::atomic<std::uint64_t> drain_truncated_thread_observation_events{0};
    std::atomic<std::uint64_t> drain_truncated_tick_events{0};
    std::atomic<std::uint64_t> retained_allocations_skipped{0};
    std::atomic<std::uint64_t> record_pool_acquisition_failures{0};
    std::atomic<std::uint64_t> insertion_contention_failures{0};
    std::atomic<std::uint64_t> exhausted_insertion_probe_failures{0};
    std::atomic<std::uint64_t> detach_contention_attempts{0};
    std::atomic<std::uint64_t> processed_allocation_events{0};
    std::atomic<std::uint64_t> processed_thread_observation_events{0};
    std::atomic<std::uint64_t> processed_tick_events{0};
    std::atomic<std::uint64_t> discarded_allocation_events{0};
    std::atomic<std::uint64_t> discarded_thread_observation_events{0};
    std::atomic<std::uint64_t> discarded_tick_events{0};
    std::atomic<std::uint64_t> consumer_lifetime_elapsed_ns{0};
    std::atomic<std::uint64_t> active_drain_elapsed_ns{0};
    std::atomic<std::uint64_t> caller_final_drain_elapsed_ns{0};
    std::atomic<std::uint64_t> caller_final_drain_allocation_events{0};
    std::atomic<std::uint64_t> caller_final_drain_thread_observation_events{0};
    std::atomic<std::uint64_t> caller_final_drain_tick_events{0};
    std::uint64_t last_module_rescan_ms = 0;

    std::array<pthread_rwlock_t, KLiveIndexShards> live_index_locks{};
    std::array<std::atomic<std::uint32_t>, KLivePresenceBuckets> live_presence{};
    pthread_mutex_t live_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_key_t thread_state_key{};
    bool thread_state_key_created = false;
    std::array<ThreadSamplingState, 2048> thread_states{};
    LiveAllocation *live_storage = nullptr;
    LiveAllocation *free_live = nullptr;
    std::atomic<LiveAllocation *> deferred_live{nullptr};
    LiveIndexEntry *live_index = nullptr;

    Impl()
    {
        for (pthread_rwlock_t &lock : live_index_locks) {
            ::pthread_rwlock_init(&lock, nullptr);
        }
    }

    ~Impl()
    {
        delete capabilities_pending.exchange(nullptr);
        delete backend_message.exchange(nullptr);
        for (pthread_rwlock_t &lock : live_index_locks) {
            ::pthread_rwlock_destroy(&lock);
        }
        ::pthread_mutex_destroy(&live_pool_mutex);
    }

    EventQueue events;
    LinuxOwnedThread aggregator_thread;
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
    test::StartFailureGate *start_failure_gate_for_testing = nullptr;
    test::LinuxAllocationTestControl *linux_control_for_testing = nullptr;
    bool force_process_event_failure_for_testing = false;
    bool fixture_no_hooks_for_testing = false;
    bool fixture_no_worker_for_testing = false;
    bool fixture_controls_configured_for_testing = false;
    test::AllocationFixtureWorkerGate *fixture_worker_gate_for_testing = nullptr;
    test::AllocationFixtureCpuWorkControl *fixture_cpu_work_for_testing = nullptr;
    ThreadSamplingState fixture_thread_state_for_testing{};
    std::uint64_t fixture_thread_owner_for_testing = 0;
    bool fixture_thread_state_active_for_testing = false;
#endif
    BoundedEventQueue<TickEvent, KTickEventCapacity> ticks;
    AllocationProfileAggregation aggregation;

    std::atomic<RecoverySink *> recovery_sink{nullptr};

    static Impl *activeContext(void *context) noexcept
    {
        auto *impl = static_cast<Impl *>(context);
        if (impl->tracking.load(std::memory_order_relaxed)) {
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
            if (impl->linux_control_for_testing != nullptr) {
                if (const auto gate = impl->linux_control_for_testing->before_hook.load(std::memory_order_acquire)) {
                    gate();
                }
            }
#endif
            impl->hot_counters[currentHookShard()].hook_calls.fetch_add(1, std::memory_order_relaxed);
        }
        return impl;
    }

    static void *hookMalloc(void *context, std::size_t size) noexcept
    {
        Impl *impl = activeContext(context);
        return impl->handleMalloc(size);
    }

    static void *hookCalloc(void *context, std::size_t count, std::size_t size) noexcept
    {
        Impl *impl = activeContext(context);
        return impl->handleCalloc(count, size);
    }

    static void *hookRealloc(void *context, void *pointer, std::size_t size) noexcept
    {
        Impl *impl = activeContext(context);
        return impl->handleRealloc(pointer, size);
    }

    static void hookFree(void *context, void *pointer) noexcept
    {
        Impl *impl = activeContext(context);
        impl->handleFree(pointer);
    }

    static void *hookReallocArray(void *context, void *pointer, std::size_t count, std::size_t size) noexcept
    {
        Impl *impl = activeContext(context);
        return impl->handleReallocArray(pointer, count, size);
    }

    static void *hookAlignedAlloc(void *context, std::size_t alignment, std::size_t size) noexcept
    {
        Impl *impl = activeContext(context);
        return impl->handleAlignedAlloc(alignment, size);
    }

    static int hookPosixMemalign(void *context, void **result, std::size_t alignment, std::size_t size) noexcept
    {
        Impl *impl = activeContext(context);
        return impl->handlePosixMemalign(result, alignment, size);
    }

    static void releaseThreadState(void *context, void *value) noexcept
    {
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        auto &impl = *static_cast<Impl *>(context);
        if (impl.linux_control_for_testing != nullptr) {
            if (const auto gate = impl.linux_control_for_testing->before_tls.load(std::memory_order_acquire)) {
                gate();
            }
        }
#else
        (void)context;
#endif
        if (value == nullptr || value == tombstonePointer()) {
            return;
        }
        auto *state = static_cast<ThreadSamplingState *>(value);
        state->inside_hook = false;
        state->tracking_suppressed = false;
        state->owner_tid.store(0, std::memory_order_relaxed);
        state->registry_state.store(0, std::memory_order_release);
    }

    ThreadSamplingState *existingThreadState() noexcept
    {
        if (!thread_state_key_created) {
            return nullptr;
        }
        auto *state = static_cast<ThreadSamplingState *>(::pthread_getspecific(thread_state_key));
        const auto address = reinterpret_cast<std::uintptr_t>(state);
        const auto begin = reinterpret_cast<std::uintptr_t>(thread_states.data());
        if (address < begin || address - begin >= sizeof(thread_states) ||
            (address - begin) % sizeof(ThreadSamplingState) != 0 ||
            state->registry_state.load(std::memory_order_acquire) != 2) {
            return nullptr;
        }
        return state;
    }

    ThreadSamplingState *currentThreadState() noexcept
    {
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        if (fixture_no_hooks_for_testing && fixture_no_worker_for_testing && fixture_thread_state_active_for_testing) {
            const auto caller_tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
            if (caller_tid != fixture_thread_owner_for_testing) {
                return nullptr;
            }
            return &fixture_thread_state_for_testing;
        }
#endif
        if (!thread_state_key_created) {
            return nullptr;
        }
        void *value = ::pthread_getspecific(thread_state_key);
        if (value == tombstonePointer()) {
            return nullptr;
        }
        auto *state = static_cast<ThreadSamplingState *>(value);
        const auto address = reinterpret_cast<std::uintptr_t>(state);
        const auto begin = reinterpret_cast<std::uintptr_t>(thread_states.data());
        const std::size_t state_limit =
            config.thread_state_limit_for_testing == 0
                ? thread_states.size()
                : (std::min)(thread_states.size(), static_cast<std::size_t>(config.thread_state_limit_for_testing));
        const std::uintptr_t end = begin + state_limit * sizeof(ThreadSamplingState);

        // pthread TLS is already scoped to the calling thread. Once a slot is
        // fully published, re-reading gettid on every allocator hook is
        // redundant and very expensive on the Linux hot path.
        if (address >= begin && address < end && state->registry_state.load(std::memory_order_acquire) == 2) {
            return state;
        }

        const auto tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
        if (address >= begin && address < end && state->registry_state.load(std::memory_order_acquire) != 0 &&
            state->owner_tid.load(std::memory_order_acquire) == tid) {
            return state;
        }

        for (std::size_t i = 0; i < state_limit; ++i) {
            ThreadSamplingState &candidate = thread_states[i];
            if (candidate.registry_state.load(std::memory_order_acquire) == 1 &&
                candidate.owner_tid.load(std::memory_order_acquire) == tid) {
                return &candidate;
            }
        }

        ThreadSamplingState *claimed = nullptr;
        for (std::size_t i = 0; i < state_limit; ++i) {
            ThreadSamplingState &candidate = thread_states[i];
            std::uint8_t expected = 0;
            if (candidate.registry_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                claimed = &candidate;
                break;
            }
        }
        if (claimed == nullptr) {
            thread_state_drops.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        claimed->owner_tid.store(tid, std::memory_order_release);
        claimed->bytes = {};
        claimed->identity_generation = 0;
        claimed->session_thread_id = 0;
        claimed->os_thread_id = tid;
        claimed->inside_hook = true;
        claimed->tracking_suppressed = false;
        claimed->identity_announced = false;
        if (::pthread_setspecific(thread_state_key, claimed) != 0) {
            claimed->inside_hook = false;
            claimed->owner_tid.store(0, std::memory_order_relaxed);
            claimed->registry_state.store(0, std::memory_order_release);
            thread_state_drops.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        claimed->inside_hook = false;
        claimed->registry_state.store(2, std::memory_order_release);
        return claimed;
    }

    bool shouldTrackCurrentThread() const noexcept
    {
        if (stop_requested.load(std::memory_order_acquire) || !tracking.load(std::memory_order_relaxed)) {
            return false;
        }
        if (config.count_only) {
            return true;
        }
        auto *self = const_cast<Impl *>(this);
        ThreadSamplingState *state = self->currentThreadState();
        return state != nullptr && !state->tracking_suppressed;
    }

    static void liveLockPause() noexcept
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

    bool tryLockLivePool() noexcept
    {
        if (config.force_live_lock_contention_for_testing) {
            return false;
        }
        for (std::size_t attempt = 0; attempt < KLiveLockAttempts; ++attempt) {
            if (::pthread_mutex_trylock(&live_pool_mutex) == 0) {
                return true;
            }
            liveLockPause();
        }
        return false;
    }

    bool tryLockLiveIndexShard(std::size_t shard) noexcept
    {
        if (config.force_live_lock_contention_for_testing) {
            return false;
        }
        for (std::size_t attempt = 0; attempt < KLiveLockAttempts; ++attempt) {
            if (::pthread_rwlock_trywrlock(&live_index_locks[shard]) == 0) {
                return true;
            }
            liveLockPause();
        }
        return false;
    }

    static std::uint64_t liveIndexHash(void *pointer) noexcept
    {
        const auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer) >> 4);
        return value * 11400714819323198485ULL;
    }

    static std::size_t liveIndexShard(std::uint64_t hash) noexcept
    {
        return static_cast<std::size_t>(hash & (KLiveIndexShards - 1));
    }

    static std::size_t livePresenceSlot(std::uint64_t hash) noexcept
    {
        return static_cast<std::size_t>((hash >> 6) & (KLivePresenceBuckets - 1));
    }

    static std::size_t liveIndexSlot(std::uint64_t hash, std::size_t shard, std::size_t offset = 0) noexcept
    {
        const std::size_t within = (static_cast<std::size_t>(hash >> 6) + offset) & (KLiveIndexShardCapacity - 1);
        return shard * KLiveIndexShardCapacity + within;
    }

    static void *entryPointer(const LiveIndexEntry &entry) noexcept
    {
        return std::atomic_ref<void *>(const_cast<void *&>(entry.pointer)).load(std::memory_order_acquire);
    }

    static LiveAllocation *entryAllocation(const LiveIndexEntry &entry) noexcept
    {
        return std::atomic_ref<LiveAllocation *>(const_cast<LiveAllocation *&>(entry.allocation))
            .load(std::memory_order_acquire);
    }

    static std::uint64_t entryAllocationId(const LiveIndexEntry &entry) noexcept
    {
        return std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t &>(entry.allocation_id))
            .load(std::memory_order_relaxed);
    }

    static void publishEntry(LiveIndexEntry &entry, void *pointer, std::uint64_t allocation_id,
                             LiveAllocation *allocation) noexcept
    {
        std::atomic_ref<std::uint64_t>(entry.allocation_id).store(allocation_id, std::memory_order_relaxed);
        std::atomic_ref<LiveAllocation *>(entry.allocation).store(allocation, std::memory_order_relaxed);
        std::atomic_ref<void *>(entry.pointer).store(pointer, std::memory_order_release);
    }

    static void clearEntry(LiveIndexEntry &entry) noexcept
    {
        std::atomic_ref<void *>(entry.pointer).store(tombstonePointer(), std::memory_order_release);
        std::atomic_ref<std::uint64_t>(entry.allocation_id).store(0, std::memory_order_relaxed);
        std::atomic_ref<LiveAllocation *>(entry.allocation).store(nullptr, std::memory_order_relaxed);
    }

    void retireAllocation(LiveAllocation *allocation, std::uint64_t released_ms) noexcept
    {
        freed_samples.fetch_add(1, std::memory_order_relaxed);
        freed_bytes.fetch_add(allocation->weight_bytes, std::memory_order_relaxed);
        live_samples.fetch_sub(1, std::memory_order_relaxed);
        live_bytes.fetch_sub(allocation->weight_bytes, std::memory_order_relaxed);
        const std::uint64_t lifetime =
            released_ms >= allocation->allocated_ms ? released_ms - allocation->allocated_ms : 0;
        lifetime_ms_total.fetch_add(lifetime, std::memory_order_relaxed);
        std::uint64_t previous = lifetime_ms_max.load(std::memory_order_relaxed);
        while (previous < lifetime &&
               !lifetime_ms_max.compare_exchange_weak(previous, lifetime, std::memory_order_relaxed)) {
        }
        recycleLiveRecord(allocation);
    }

    LiveAllocation *detachAllocation(void *pointer) noexcept
    {
        if (pointer == nullptr || live_index == nullptr) {
            return nullptr;
        }
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t presence_slot = livePresenceSlot(hash);
        if (!config.force_live_lock_contention_for_testing &&
            live_presence[presence_slot].load(std::memory_order_acquire) == 0) {
            return nullptr;
        }
        const std::size_t shard = liveIndexShard(hash);
        if (!tryLockLiveIndexShard(shard)) {
            detach_contention_attempts.fetch_add(1, std::memory_order_relaxed);
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        lifecycle_writers.fetch_add(1, std::memory_order_acq_rel);
        lifecycle_version.fetch_add(1, std::memory_order_release);
        LiveAllocation *detached = nullptr;
        for (std::size_t offset = 0; offset < KLiveIndexShardCapacity; ++offset) {
            LiveIndexEntry &entry = live_index[liveIndexSlot(hash, shard, offset)];
            void *entry_pointer = entryPointer(entry);
            if (entry_pointer == nullptr) {
                break;
            }
            if (entry_pointer == pointer) {
                LiveAllocation *entry_allocation = entryAllocation(entry);
                if (entry_allocation == nullptr || entryAllocationId(entry) != entry_allocation->allocation_id) {
                    lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                detached = entry_allocation;
                clearEntry(entry);
                live_presence[presence_slot].fetch_sub(1, std::memory_order_release);
                break;
            }
        }
        lifecycle_version.fetch_add(1, std::memory_order_release);
        lifecycle_writers.fetch_sub(1, std::memory_order_release);
        ::pthread_rwlock_unlock(&live_index_locks[shard]);
        return detached;
    }

    void restoreDetachedAllocation(LiveAllocation *allocation) noexcept
    {
        if (allocation == nullptr) {
            return;
        }
        const std::uint64_t weight = allocation->weight_bytes;
        if (!insertLiveAllocation(allocation, false)) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            live_samples.fetch_sub(1, std::memory_order_relaxed);
            live_bytes.fetch_sub(weight, std::memory_order_relaxed);
            recycleLiveRecord(allocation);
        }
    }

    struct OriginalErrno {
        int incoming = errno;
        int result = incoming;

        ~OriginalErrno() { errno = result; }

        template <typename Function, typename... Args>
        auto invoke(Function function, Args... args) noexcept -> decltype(function(args...))
        {
            errno = incoming;
            if constexpr (std::is_void_v<decltype(function(args...))>) {
                function(args...);
                result = errno;
            }
            else {
                auto value = function(args...);
                result = errno;
                return value;
            }
        }
    };

    void handleFree(void *pointer) noexcept
    {
        OriginalErrno call;
        if (!shouldTrackCurrentThread()) {
            call.invoke(real_free, pointer);
            return;
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            call.invoke(real_free, pointer);
            return;
        }
        LiveAllocation *allocation = detachAllocation(pointer);
        call.invoke(real_free, pointer);
        if (allocation != nullptr) {
            retireAllocation(allocation, monotonicMs());
        }
    }

    void *handleMalloc(std::size_t size) noexcept
    {
        OriginalErrno call;
        if (!shouldTrackCurrentThread()) {
            return call.invoke(real_malloc, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return call.invoke(real_malloc, size);
        }
        void *result = call.invoke(real_malloc, size);
        if (result != nullptr) {
            recordAllocation(result, static_cast<std::uint64_t>(size));
        }
        return result;
    }

    void *handleCalloc(std::size_t count, std::size_t size) noexcept
    {
        OriginalErrno call;
        if (!shouldTrackCurrentThread()) {
            return call.invoke(real_calloc, count, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return call.invoke(real_calloc, count, size);
        }
        void *result = call.invoke(real_calloc, count, size);
        std::uint64_t bytes = 0;
        if (result != nullptr && checkedMultiply(count, size, bytes)) {
            recordAllocation(result, bytes);
        }
        return result;
    }

    void *handleRealloc(void *pointer, std::size_t size) noexcept
    {
        OriginalErrno call;
        if (!shouldTrackCurrentThread()) {
            return call.invoke(real_realloc, pointer, size);
        }
        TrackingCallGuard tracking_guard(*this);
        if (!tracking_guard) {
            return call.invoke(real_realloc, pointer, size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return call.invoke(real_realloc, pointer, size);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *result = call.invoke(real_realloc, pointer, size);
        const bool replaced = result != nullptr || (pointer != nullptr && size == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (result != nullptr && size != 0) {
            recordAllocation(result, static_cast<std::uint64_t>(size));
        }
        return result;
    }

    void *handleReallocArray(void *pointer, std::size_t count, std::size_t size) noexcept
    {
        OriginalErrno call;
        if (!shouldTrackCurrentThread()) {
            return call.invoke(real_reallocarray, pointer, count, size);
        }
        TrackingCallGuard tracking_guard(*this);
        if (!tracking_guard) {
            return call.invoke(real_reallocarray, pointer, count, size);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, size, bytes);
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return call.invoke(real_reallocarray, pointer, count, size);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *result = call.invoke(real_reallocarray, pointer, count, size);
        const bool replaced = result != nullptr || (pointer != nullptr && valid_size && bytes == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (result != nullptr && valid_size && bytes != 0) {
            recordAllocation(result, bytes);
        }
        return result;
    }

    void *handleAlignedAlloc(std::size_t alignment, std::size_t size) noexcept
    {
        OriginalErrno call;
        if (!shouldTrackCurrentThread()) {
            return call.invoke(real_aligned_alloc, alignment, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return call.invoke(real_aligned_alloc, alignment, size);
        }
        void *result = call.invoke(real_aligned_alloc, alignment, size);
        if (result != nullptr) {
            recordAllocation(result, static_cast<std::uint64_t>(size));
        }
        return result;
    }

    int handlePosixMemalign(void **result_pointer, std::size_t alignment, std::size_t size) noexcept
    {
        OriginalErrno call;
        if (!shouldTrackCurrentThread()) {
            return call.invoke(real_posix_memalign, result_pointer, alignment, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return call.invoke(real_posix_memalign, result_pointer, alignment, size);
        }
        const int result = call.invoke(real_posix_memalign, result_pointer, alignment, size);
        if (result == 0 && result_pointer != nullptr && *result_pointer != nullptr) {
            recordAllocation(*result_pointer, static_cast<std::uint64_t>(size));
        }
        return result;
    }

    LiveAllocation *acquireLiveRecord() noexcept
    {
        if (!tryLockLivePool()) {
            record_pool_acquisition_failures.fetch_add(1, std::memory_order_relaxed);
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (free_live == nullptr && lifecycle_readers.load(std::memory_order_acquire) == 0) {
            free_live = deferred_live.exchange(nullptr, std::memory_order_acq_rel);
        }
        LiveAllocation *record = free_live;
        if (record != nullptr) {
            free_live = record->next;
            record->next = nullptr;
        }
        ::pthread_mutex_unlock(&live_pool_mutex);
        if (record == nullptr) {
            record_pool_acquisition_failures.fetch_add(1, std::memory_order_relaxed);
        }
        return record;
    }

    void accountLiveAllocation(std::uint64_t weight) noexcept
    {
        const std::uint64_t current_live = live_samples.fetch_add(1, std::memory_order_relaxed) + 1;
        live_bytes.fetch_add(weight, std::memory_order_relaxed);
        std::uint64_t previous_peak = peak_live_samples.load(std::memory_order_relaxed);
        while (previous_peak < current_live &&
               !peak_live_samples.compare_exchange_weak(previous_peak, current_live, std::memory_order_relaxed)) {
        }
    }

    bool insertLiveAllocation(LiveAllocation *allocation, bool account_live) noexcept
    {
        void *pointer = allocation->pointer;
        const std::uint64_t allocation_id = allocation->allocation_id;
        const std::uint64_t weight = allocation->weight_bytes;
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t presence_slot = livePresenceSlot(hash);
        const std::size_t shard = liveIndexShard(hash);
        if (!tryLockLiveIndexShard(shard)) {
            insertion_contention_failures.fetch_add(1, std::memory_order_relaxed);
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        lifecycle_writers.fetch_add(1, std::memory_order_acq_rel);
        lifecycle_version.fetch_add(1, std::memory_order_release);
        LiveAllocation *replaced = nullptr;
        bool inserted = false;
        bool added_presence = false;
        std::size_t tombstone = KLiveIndexCapacity;
        for (std::size_t offset = 0; offset < KLiveIndexShardCapacity; ++offset) {
            const std::size_t slot = liveIndexSlot(hash, shard, offset);
            LiveIndexEntry &entry = live_index[slot];
            void *entry_pointer = entryPointer(entry);
            if (entry_pointer == tombstonePointer()) {
                if (tombstone == KLiveIndexCapacity) {
                    tombstone = slot;
                }
                continue;
            }
            if (entry_pointer == pointer) {
                replaced = entryAllocation(entry);
                std::atomic_ref<void *>(entry.pointer).store(tombstonePointer(), std::memory_order_release);
                if (account_live) {
                    accountLiveAllocation(weight);
                }
                publishEntry(entry, pointer, allocation_id, allocation);
                inserted = true;
                break;
            }
            if (entry_pointer == nullptr) {
                LiveIndexEntry &destination = live_index[tombstone != KLiveIndexCapacity ? tombstone : slot];
                if (account_live) {
                    accountLiveAllocation(weight);
                }
                publishEntry(destination, pointer, allocation_id, allocation);
                inserted = true;
                added_presence = true;
                break;
            }
        }
        if (!inserted && tombstone != KLiveIndexCapacity) {
            if (account_live) {
                accountLiveAllocation(weight);
            }
            publishEntry(live_index[tombstone], pointer, allocation_id, allocation);
            inserted = true;
            added_presence = true;
        }
        if (!inserted) {
            exhausted_insertion_probe_failures.fetch_add(1, std::memory_order_relaxed);
        }
        if (added_presence) {
            live_presence[presence_slot].fetch_add(1, std::memory_order_release);
        }
        lifecycle_version.fetch_add(1, std::memory_order_release);
        lifecycle_writers.fetch_sub(1, std::memory_order_release);
        ::pthread_rwlock_unlock(&live_index_locks[shard]);

        if (replaced != nullptr) {
            // Pointer reuse retires lifecycle records missed by patched imports.
            retireAllocation(replaced, monotonicMs());
        }
        return inserted;
    }

    void recycleLiveRecord(LiveAllocation *allocation) noexcept
    {
        if (lifecycle_readers.load(std::memory_order_acquire) != 0) {
            LiveAllocation *head = deferred_live.load(std::memory_order_relaxed);
            do {
                allocation->next = head;
            } while (!deferred_live.compare_exchange_weak(head, allocation, std::memory_order_release,
                                                          std::memory_order_relaxed));
            return;
        }
        if (!tryLockLivePool()) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        allocation->next = free_live;
        free_live = allocation;
        ::pthread_mutex_unlock(&live_pool_mutex);
    }

    void recordAllocation(void *pointer, std::uint64_t requested_bytes) noexcept
    {
        HotCounters &counters = hot_counters[currentHookShard()];
        counters.successful_allocation_calls.fetch_add(1, std::memory_order_relaxed);
        if (requested_bytes == 0) {
            return;
        }
        counters.observed_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);
        if (config.count_only) {
            return;
        }
        ThreadSamplingState *thread_pointer = currentThreadState();
        if (thread_pointer == nullptr) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ThreadSamplingState &thread = *thread_pointer;
        ByteSamplingState &state = thread.bytes;
        const std::uint64_t current_generation = generation.load(std::memory_order_relaxed);
        const std::uint64_t interval = interval_bytes.load(std::memory_order_relaxed);
        const std::uint64_t current_tid = thread.owner_tid.load(std::memory_order_relaxed);
        if (state.generation != current_generation) {
            resetByteSamplingState(state, current_generation,
                                   sampling_seed.load(std::memory_order_relaxed) ^ current_generation ^ current_tid,
                                   interval);
        }
        if (thread.identity_generation != current_generation) {
            thread.identity_generation = current_generation;
            thread.os_thread_id = current_tid;
            const std::uint64_t next = next_session_thread_id.fetch_add(1, std::memory_order_relaxed);
            thread.session_thread_id = next;
            thread.identity_announced = false;
            if (next <= KMaxSampledThreads) {
                registered_threads.fetch_add(1, std::memory_order_relaxed);
            }
            else {
                overflow_threads.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const std::uint64_t points = consumeSampledBytes(state, requested_bytes, interval);
        if (points == 0) {
            return;
        }
        sampling_points.fetch_add(points, std::memory_order_relaxed);

        LiveAllocation *allocation = acquireLiveRecord();
        if (allocation == nullptr) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        allocation->pointer = pointer;
        allocation->allocation_id = next_allocation_id.fetch_add(1, std::memory_order_relaxed);
        allocation->weight_bytes = saturatingMultiply(points, interval);
        allocation->requested_bytes = requested_bytes;
        allocation->allocated_ms = monotonicMs();
        allocation->tick_id = current_tick.load(std::memory_order_relaxed);
        allocation->thread_id = thread.session_thread_id;
        allocation->os_thread_id = thread.os_thread_id;
        allocation->window = profiling_window::windowNow();
        allocation->depth = static_cast<std::uint16_t>(
            cpptrace::safe_generate_raw_trace(allocation->frames, KStackDepth, KFramesToSkip));
        const std::uint64_t allocation_weight = allocation->weight_bytes;
        const std::uint64_t allocation_tick = allocation->tick_id;
        const std::uint64_t allocation_thread = allocation->thread_id;
        const std::uint64_t allocation_os_thread = allocation->os_thread_id;
        const std::int32_t allocation_window = allocation->window;
        const std::uint16_t allocation_depth = allocation->depth;
        const bool live_only = config.live_only;
        AllocationEvent event{};
        event.thread_id = allocation_thread;
        event.os_thread_id = allocation_os_thread;
        event.thread_observation = live_only;
        if (!live_only) {
            event.weight_bytes = allocation_weight;
            event.tick_id = allocation_tick;
            event.window = allocation_window;
            event.depth = allocation_depth;
            std::memcpy(event.frames, allocation->frames,
                        static_cast<std::size_t>(allocation_depth) * sizeof(cpptrace::frame_ptr));
        }
        const bool inserted = allocation_depth != 0 && insertLiveAllocation(allocation, true);
        if (!inserted) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            recycleLiveRecord(allocation);
            return;
        }

        if (live_only) {
            if (!thread.identity_announced) {
                if (events.enqueue(event)) {
                    thread.identity_announced = true;
                }
                else {
                    dropped_samples.fetch_add(1, std::memory_order_relaxed);
                    dropped_events.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        if (!events.enqueue(event)) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            dropped_events.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        enqueued_samples.fetch_add(1, std::memory_order_relaxed);
    }

    template <typename Function>
    bool resolveAllocator(const char *name, Function &function, bool required, std::string &error)
    {
        ::dlerror();
        // Preserve LD_PRELOAD interposition when resolving the effective allocator.
        function = reinterpret_cast<Function>(::dlsym(RTLD_DEFAULT, name));
        const char *failure = ::dlerror();
        if (function == nullptr && required) {
            error = std::string("required Linux allocator symbol not found: ") + name;
            if (failure != nullptr) {
                error += ": ";
                error += failure;
            }
            return false;
        }
        return true;
    }

    bool allocateLifecycleStorage(std::string &error)
    {
        void *records = ::mmap(nullptr, sizeof(LiveAllocation) * KEventCapacity, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void *index = ::mmap(nullptr, sizeof(LiveIndexEntry) * KLiveIndexCapacity, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (records == MAP_FAILED || index == MAP_FAILED) {
            if (records != MAP_FAILED) {
                ::munmap(records, sizeof(LiveAllocation) * KEventCapacity);
            }
            if (index != MAP_FAILED) {
                ::munmap(index, sizeof(LiveIndexEntry) * KLiveIndexCapacity);
            }
            error = "mmap for Linux allocation lifecycle tracking failed: " + std::string(std::strerror(errno));
            return false;
        }
        live_storage = static_cast<LiveAllocation *>(records);
        live_index = static_cast<LiveIndexEntry *>(index);
        free_live = nullptr;
        deferred_live.store(nullptr, std::memory_order_relaxed);
        for (std::size_t i = 0; i < KEventCapacity; ++i) {
            live_storage[i].next = free_live;
            free_live = &live_storage[i];
        }
        return true;
    }

    void releaseLifecycleStorage() noexcept
    {
        if (live_storage != nullptr) {
            ::munmap(live_storage, sizeof(LiveAllocation) * KEventCapacity);
            live_storage = nullptr;
        }
        if (live_index != nullptr) {
            ::munmap(live_index, sizeof(LiveIndexEntry) * KLiveIndexCapacity);
            live_index = nullptr;
        }
        free_live = nullptr;
        deferred_live.store(nullptr, std::memory_order_relaxed);
    }

    bool prepareHooks(std::string &error)
    {
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        if (fixture_no_hooks_for_testing) {
            return true;
        }
#endif
        if (real_malloc == nullptr) {
            if (!resolveAllocator("malloc", real_malloc, true, error) ||
                !resolveAllocator("calloc", real_calloc, true, error) ||
                !resolveAllocator("realloc", real_realloc, true, error) ||
                !resolveAllocator("free", real_free, true, error) ||
                !resolveAllocator("reallocarray", real_reallocarray, true, error) ||
                !resolveAllocator("aligned_alloc", real_aligned_alloc, true, error) ||
                !resolveAllocator("posix_memalign", real_posix_memalign, true, error)) {
                return false;
            }
        }

        const std::array originals{
            reinterpret_cast<void *>(real_malloc),        reinterpret_cast<void *>(real_calloc),
            reinterpret_cast<void *>(real_realloc),       reinterpret_cast<void *>(real_free),
            reinterpret_cast<void *>(real_reallocarray),  reinterpret_cast<void *>(real_aligned_alloc),
            reinterpret_cast<void *>(real_posix_memalign)};
        if (!gateway.reserve(originals, error)) {
            return false;
        }
        backend_cleanup_pending.store(true, std::memory_order_release);
        if (!thread_state_key_created) {
            if (!gateway.open(gatewayCallbacks(), this, true, error)) {
                return false;
            }
            gateway.publish();
            gateway_published = true;
            int key_result = 0;
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
            if (linux_control_for_testing != nullptr && linux_control_for_testing->start_failure.load() == 1) {
                key_result = EAGAIN;
            }
            else
#endif
            {
                key_result = ::pthread_key_create(&thread_state_key, gateway.binding().tls_entry);
            }
            if (key_result != 0) {
                error = "pthread_key_create for allocation thread state failed";
                return false;
            }
            thread_state_key_created = true;
        }
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        if (linux_control_for_testing != nullptr) {
            hooks.setScanModuleGateForTesting(linux_control_for_testing->scan_module);
            const auto failure = linux_control_for_testing->start_failure.load();
            if (failure == 2 || failure == 3) {
                error = "injected post-key allocation hook preparation failure";
                return false;
            }
        }
#endif
        const auto &entries = gateway.binding().entries;
        const std::array specs{
            ElfImportHookSpec{.name = "malloc", .replacement = entries[0], .required = true, .original = originals[0]},
            ElfImportHookSpec{.name = "calloc", .replacement = entries[1], .required = true, .original = originals[1]},
            ElfImportHookSpec{.name = "realloc", .replacement = entries[2], .required = true, .original = originals[2]},
            ElfImportHookSpec{.name = "free", .replacement = entries[3], .required = true, .original = originals[3]},
            ElfImportHookSpec{
                .name = "reallocarray", .replacement = entries[4], .required = false, .original = originals[4]},
            ElfImportHookSpec{
                .name = "aligned_alloc", .replacement = entries[5], .required = false, .original = originals[5]},
            ElfImportHookSpec{
                .name = "posix_memalign", .replacement = entries[6], .required = false, .original = originals[6]},
        };
        if (!hooks.prepare(specs, error)) {
            return false;
        }
        updateHookCapabilities();

        if (!hooks.installed()) {
            cpptrace::frame_ptr warm[8]{};
            cpptrace::safe_generate_raw_trace(warm, 8, 0);
            if (warm[0] != 0) {
                cpptrace::safe_object_frame object;
                cpptrace::get_safe_object_frame(warm[0], &object);
            }
        }
        return true;
    }

    static SparkGatewayCallbacksV1 gatewayCallbacks() noexcept
    {
        return {&hookMalloc,       &hookCalloc,       &hookRealloc,       &hookFree,
                &hookReallocArray, &hookAlignedAlloc, &hookPosixMemalign, &releaseThreadState};
    }

    void updateHookCapabilities()
    {
        auto mailbox = std::make_unique<CapabilityMailbox>();
        mailbox->epoch = generation.load(std::memory_order_relaxed);
        for (const ElfImportHookCapability &capability : hooks.capabilities()) {
            mailbox->values.push_back(
                {capability.name, capability.available ? AllocationHookStatus::Active : AllocationHookStatus::Missing,
                 capability.detail});
        }
        delete capabilities_pending.exchange(mailbox.release(), std::memory_order_acq_rel);
        publishHookDiagnostics();
    }

    void publishHookDiagnostics() noexcept
    {
        hooks_installed.store(hooks.installed(), std::memory_order_release);
        hook_target_count.store(hooks.targetCount(), std::memory_order_release);
        failed_module_count.store(hooks.failedModuleCount(), std::memory_order_release);
        hooked_module_count.store(hooks.hookedModuleCount(), std::memory_order_release);
        skipped_module_count.store(hooks.skippedModuleCount(), std::memory_order_release);
    }

    void consumeHookCapabilities()
    {
        std::unique_ptr<CapabilityMailbox> mailbox(capabilities_pending.exchange(nullptr, std::memory_order_acq_rel));
        if (mailbox != nullptr && mailbox->epoch == generation.load(std::memory_order_relaxed)) {
            hook_capabilities = std::move(mailbox->values);
        }
    }

    bool installHooks(std::string &error)
    {
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        if (fixture_no_hooks_for_testing) {
            return true;
        }
#endif
        Impl *expected = nullptr;
        if (!mActiveInstance.compare_exchange_strong(expected, this, std::memory_order_release,
                                                     std::memory_order_relaxed) &&
            expected != this) {
            error = "another native allocation sampler backend is already active";
            return false;
        }
        if (!gateway.open(gatewayCallbacks(), this, false, error)) {
            return false;
        }
        gateway.publish();
        gateway_published = true;
        if (!hooks.install(error)) {
            gateway.close(false);
            return false;
        }
        publishHookDiagnostics();
        return true;
    }

    template <std::size_t N>
    static bool waitFor(const std::array<std::atomic<std::uint64_t>, N> &counters, const char *description,
                        std::string &error, std::chrono::steady_clock::time_point deadline) noexcept
    {
        const bool quiesced = detail::waitForQuiescence<std::chrono::steady_clock>(
            std::max(std::chrono::steady_clock::duration::zero(), deadline - std::chrono::steady_clock::now()),
            [&counters] {
                return std::ranges::any_of(
                    counters, [](const auto &counter) { return counter.load(std::memory_order_acquire) != 0; });
            },
            [](std::chrono::steady_clock::duration) {
                const timespec delay{.tv_sec = 0, .tv_nsec = 1000000};
                (void)::nanosleep(&delay, nullptr);
            });
        if (quiesced) {
            return true;
        }
        try {
            error = std::string("timed out waiting for ") + description + " to quiesce";
        }
        catch (...) {
            error.clear();
        }
        return false;
    }

    bool backendCleanupPending() const noexcept
    {
        return backend_cleanup_pending.load(std::memory_order_acquire) ||
               backend_shutdown_pending.load(std::memory_order_acquire) ||
               failed_start_pending.load(std::memory_order_acquire) || aggregator_thread.joinable();
    }

    FrameKey frameKey(cpptrace::frame_ptr address)
    {
        cpptrace::safe_object_frame object;
        cpptrace::get_safe_object_frame(address, &object);
        std::string_view path =
            object.object_path[0] != '\0' ? std::string_view(object.object_path) : std::string_view("unknown");
        return aggregation.internFrame(path, static_cast<std::uint64_t>(object.address_relative_to_object_start),
                                       static_cast<std::uint64_t>(object.raw_address));
    }

    bool buildSample(const cpptrace::frame_ptr *frames, std::uint16_t depth, std::uint64_t tick_id,
                     std::uint64_t thread_id, std::uint64_t os_thread_id, std::int32_t window, std::uint64_t weight,
                     Sample &sample)
    {
        sample.tick_id = tick_id;
        const AllocationThreadSelection selection = aggregation.resolveThread(thread_id, os_thread_id);
        if (!selection.selected) {
            filtered_samples.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        sample.thread_id = selection.profile_thread_id;
        sample.os_thread_id = os_thread_id;
        sample.thread_name = selection.display_name;
        sample.window = window;
        sample.weight = weight;
        sample.frames.reserve(depth);
        for (std::size_t i = 0; i < depth; ++i) {
            if (frames[i] != 0) {
                sample.frames.push_back(frameKey(frames[i]));
            }
        }
        if (sample.frames.empty()) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    bool buildSnapshotSample(const cpptrace::frame_ptr *frames, std::uint16_t depth, std::uint64_t tick_id,
                             std::uint64_t thread_id, std::uint64_t os_thread_id, std::int32_t window,
                             std::uint64_t weight, Sample &sample)
    {
        sample.tick_id = tick_id;
        const AllocationThreadSelection selection = aggregation.resolveThread(thread_id, os_thread_id);
        if (!selection.selected) {
            return false;
        }
        sample.thread_id = selection.profile_thread_id;
        sample.os_thread_id = os_thread_id;
        sample.thread_name = selection.display_name;
        sample.window = window;
        sample.weight = weight;
        sample.frames.reserve(depth);
        for (std::size_t i = 0; i < depth; ++i) {
            if (frames[i] != 0) {
                sample.frames.push_back(frameKey(frames[i]));
            }
        }
        return !sample.frames.empty();
    }

    enum class DrainContext {
        Aggregator,
        CallerSnapshot,
        CallerFinal,
    };

    void markAccountingFailure() noexcept
    {
        accounting_state.store(AllocationAccountingState::Failed, std::memory_order_release);
    }

    void markStopIncomplete() noexcept
    {
        AllocationAccountingState expected = AllocationAccountingState::Active;
        accounting_state.compare_exchange_strong(expected, AllocationAccountingState::Incomplete,
                                                 std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void publishComplete() noexcept
    {
        AllocationAccountingState state = accounting_state.load(std::memory_order_acquire);
        while (state != AllocationAccountingState::Failed && state != AllocationAccountingState::NotApplicable &&
               state != AllocationAccountingState::NotStarted && state != AllocationAccountingState::Complete &&
               !accounting_state.compare_exchange_weak(state, AllocationAccountingState::Complete,
                                                       std::memory_order_release, std::memory_order_acquire)) {
        }
    }

    class StartAttemptScope {
    public:
        explicit StartAttemptScope(Impl &impl) noexcept : impl_(impl) {}

        ~StartAttemptScope()
        {
            if (armed_) {
                impl_.markAccountingFailure();
            }
        }

        void dismiss() noexcept { armed_ = false; }

        StartAttemptScope(const StartAttemptScope &) = delete;
        StartAttemptScope &operator=(const StartAttemptScope &) = delete;

    private:
        Impl &impl_;
        bool armed_ = true;
    };

    class AccountingFailureScope {
    public:
        explicit AccountingFailureScope(Impl &impl) noexcept : impl_(impl), uncaught_(std::uncaught_exceptions()) {}

        ~AccountingFailureScope() noexcept
        {
            if (std::uncaught_exceptions() > uncaught_) {
                impl_.markAccountingFailure();
            }
        }

        AccountingFailureScope(const AccountingFailureScope &) = delete;
        AccountingFailureScope &operator=(const AccountingFailureScope &) = delete;

    private:
        Impl &impl_;
        int uncaught_ = 0;
    };

    class DrainElapsedScope {
    public:
        DrainElapsedScope(Impl &impl, DrainContext context) noexcept
            : impl_(impl), context_(context), started_ns_(impl.config.count_only ? 0 : monotonicNs()),
              uncaught_(std::uncaught_exceptions())
        {
        }

        ~DrainElapsedScope() noexcept
        {
            if (!impl_.config.count_only) {
                const std::uint64_t elapsed_ns = monotonicNs() - started_ns_;
                if (context_ == DrainContext::CallerFinal) {
                    impl_.caller_final_drain_elapsed_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
                }
                else if (context_ == DrainContext::Aggregator) {
                    impl_.active_drain_elapsed_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
                }
            }
            if (std::uncaught_exceptions() > uncaught_) {
                impl_.markAccountingFailure();
            }
        }

        DrainElapsedScope(const DrainElapsedScope &) = delete;
        DrainElapsedScope &operator=(const DrainElapsedScope &) = delete;

    private:
        Impl &impl_;
        DrainContext context_;
        std::uint64_t started_ns_;
        int uncaught_ = 0;
    };

    class ConsumerLifetimeScope {
    public:
        explicit ConsumerLifetimeScope(Impl &impl) noexcept : impl_(impl), started_ns_(monotonicNs()) {}

        ~ConsumerLifetimeScope() noexcept
        {
            impl_.consumer_lifetime_elapsed_ns.store(monotonicNs() - started_ns_, std::memory_order_release);
        }

        ConsumerLifetimeScope(const ConsumerLifetimeScope &) = delete;
        ConsumerLifetimeScope &operator=(const ConsumerLifetimeScope &) = delete;

    private:
        Impl &impl_;
        std::uint64_t started_ns_;
    };

    void processEvent(const AllocationEvent &event, DrainContext context)
    {
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        if (linux_control_for_testing != nullptr && linux_control_for_testing->before_event != nullptr) {
            linux_control_for_testing->before_event();
        }
#endif
        const bool caller_final_drain = context == DrainContext::CallerFinal;
        if (event.thread_observation) {
            processed_thread_observation_events.fetch_add(1, std::memory_order_relaxed);
            if (caller_final_drain) {
                caller_final_drain_thread_observation_events.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else {
            processed_allocation_events.fetch_add(1, std::memory_order_relaxed);
            if (caller_final_drain) {
                caller_final_drain_allocation_events.fetch_add(1, std::memory_order_relaxed);
            }
        }
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        if (force_process_event_failure_for_testing) {
            throw std::runtime_error("injected allocation event processing failure");
        }
#endif
        if (event.thread_observation) {
            aggregation.observeThread(event.thread_id, event.os_thread_id);
            return;
        }
        Sample sample;
        if (!buildSample(event.frames, event.depth, event.tick_id, event.thread_id, event.os_thread_id, event.window,
                         event.weight_bytes, sample)) {
            return;
        }
        (void)aggregation.processSample(std::move(sample));
    }

    void finalizeLiveProfile()
    {
        AccountingFailureScope failure_scope(*this);
        const std::uint64_t stopped_ms = terminal_ms.load(std::memory_order_acquire);
        const std::uint64_t terminal = terminal_tick.load(std::memory_order_acquire);
        std::uint64_t total_age = 0;
        std::uint64_t maximum_age = 0;
        for (std::size_t i = 0; i < KLiveIndexCapacity; ++i) {
            const LiveIndexEntry &entry = live_index[i];
            void *pointer = entryPointer(entry);
            LiveAllocation *entry_allocation = entryAllocation(entry);
            if (pointer == nullptr || pointer == tombstonePointer() || entry_allocation == nullptr) {
                continue;
            }
            const LiveAllocation &allocation = *entry_allocation;
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
            if (linux_control_for_testing != nullptr && linux_control_for_testing->before_final_record != nullptr) {
                linux_control_for_testing->before_final_record();
            }
#endif
            if (!aggregation.tickAccepts(allocation.tick_id)) {
                aggregation.classifyFinalTickSamples(allocation.tick_id, terminal, 1);
                continue;
            }
            Sample sample;
            if (buildSample(allocation.frames, allocation.depth, allocation.tick_id, allocation.thread_id,
                            allocation.os_thread_id, allocation.window, allocation.weight_bytes, sample)) {
                const std::uint64_t age =
                    stopped_ms >= allocation.allocated_ms ? stopped_ms - allocation.allocated_ms : 0;
                total_age += age;
                maximum_age = (std::max)(maximum_age, age);
                (void)aggregation.acceptLiveSample(std::move(sample));
            }
        }
        retained_age_ms_total.store(total_age, std::memory_order_relaxed);
        retained_age_ms_max.store(maximum_age, std::memory_order_relaxed);
    }

    void drainQueues(DrainContext context)
    {
        DrainElapsedScope elapsed(*this, context);
        TickEvent tick;
        while (ticks.dequeue(tick)) {
            processed_tick_events.fetch_add(1, std::memory_order_relaxed);
            if (context == DrainContext::CallerFinal) {
                caller_final_drain_tick_events.fetch_add(1, std::memory_order_relaxed);
            }
            aggregation.processTick(tick.tick_id, tick.mspt_ms);
        }
        if (events.storage == nullptr) {
            return;
        }
        AllocationEvent event;
        while (events.dequeue(event)) {
            processEvent(event, context);
        }
    }

    void discardResidualTicks() noexcept
    {
        TickEvent tick;
        for (std::size_t attempts = 0; attempts < KTickEventCapacity && ticks.dequeue(tick); ++attempts) {
            discarded_tick_events.fetch_add(1, std::memory_order_relaxed);
            drain_truncated_tick_events.fetch_add(1, std::memory_order_relaxed);
            caller_final_drain_tick_events.fetch_add(1, std::memory_order_relaxed);
            drain_truncated.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void requestFinalization()
    {
        tick_admission_open.store(false, std::memory_order_release);
        if (!finalize_pending.exchange(true, std::memory_order_acq_rel)) {
            terminal_ms.store(monotonicMs(), std::memory_order_release);
        }
    }

    void finishAggregationIfNeeded()
    {
        std::scoped_lock lock(aggregate_mutex);
        AccountingFailureScope failure_scope(*this);
        drainQueues(DrainContext::CallerFinal);
        if (finalize_pending.load(std::memory_order_acquire) && !pending_finalized.load(std::memory_order_acquire)) {
            aggregation.finishPending(terminal_tick.load(std::memory_order_acquire));
            pending_finalized.store(true, std::memory_order_release);
        }
    }

    void aggregatorLoop()
    {
        ConsumerLifetimeScope lifetime(*this);
        while (!session_ready.load(std::memory_order_acquire)) {
            if (!aggregator_running.load(std::memory_order_acquire)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        if (fixture_worker_gate_for_testing != nullptr) {
            fixture_worker_gate_for_testing->entered.store(true, std::memory_order_release);
            if (!waitFixtureWorkerGate(*fixture_worker_gate_for_testing)) {
                throw std::runtime_error("allocation fixture worker synchronization failed");
            }
        }
#endif
        TrackingSuppressionGuard suppress(*this);
        if (config.fail_aggregator_for_testing) {
            throw std::runtime_error("injected allocation aggregator failure");
        }
        if (config.hold_aggregator_until_event_drop_for_testing) {
            while (aggregator_running.load(std::memory_order_acquire) &&
                   dropped_events.load(std::memory_order_acquire) == 0) {
                std::this_thread::yield();
            }
        }
        if (config.aggregator_delay_ms_for_testing != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config.aggregator_delay_ms_for_testing));
        }
        while (aggregator_running.load(std::memory_order_acquire)) {
            {
                std::scoped_lock lock(aggregate_mutex);
                drainQueues(DrainContext::Aggregator);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::scoped_lock lock(aggregate_mutex);
            drainQueues(DrainContext::Aggregator);
            if (finalize_pending.load(std::memory_order_acquire)) {
                aggregation.finishPending(terminal_tick.load(std::memory_order_acquire));
                pending_finalized.store(true, std::memory_order_release);
            }
            if (config.live_only && live_index != nullptr) {
                finalizeLiveProfile();
            }
            discardResidualTicks();
        }
    }

    bool captureSnapshot(AllocationSnapshot &snapshot, std::string &error)
    {
        ConsumerGuard consumer(*this);
        if (!consumer) {
            error.clear();
            return false;
        }
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        if (linux_control_for_testing != nullptr && linux_control_for_testing->snapshot_admitted != nullptr) {
            linux_control_for_testing->snapshot_admitted();
        }
#endif
        TrackingSuppressionGuard suppress(*this, true);
        AccountingFailureScope failure_scope(*this);
        std::scoped_lock lifecycle_lock(lifecycle_mutex);
        error.clear();
        if (!running.load(std::memory_order_acquire)) {
            return false;
        }
        if (aggregator_failed.load(std::memory_order_acquire)) {
            error = "allocation aggregator failed: " + aggregatorFailureMessage();
            return false;
        }

#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        if (linux_control_for_testing != nullptr && linux_control_for_testing->snapshot_before_aggregate != nullptr) {
            linux_control_for_testing->snapshot_before_aggregate();
        }
#endif
        std::unique_lock aggregate_lock(aggregate_mutex, std::defer_lock);
        if (!aggregate_lock.try_lock_for(std::chrono::seconds(5))) {
            error = "timed out waiting for the allocation aggregator snapshot";
            return false;
        }
        drainQueues(DrainContext::CallerSnapshot);
        snapshot = AllocationSnapshot{};
        snapshot.number_of_ticks = current_tick.load(std::memory_order_relaxed);

        if (!config.live_only) {
            return aggregation.copyCumulativeSnapshot(snapshot, current_tick.load(std::memory_order_relaxed), error);
        }

        std::vector<LiveAllocation> retained;
        retained.reserve(
            (std::min)(KEventCapacity, static_cast<std::size_t>(live_samples.load(std::memory_order_relaxed))));
        const auto retained_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        lifecycle_readers.fetch_add(1, std::memory_order_acq_rel);
        bool stable = false;
        try {
            stable = detail::captureStableShardSnapshot(
                KLiveIndexShards, retained_deadline, [&retained] { retained.clear(); },
                [this] { return lifecycle_version.load(std::memory_order_acquire); },
                [this](std::size_t shard) { return ::pthread_rwlock_tryrdlock(&live_index_locks[shard]) == 0; },
                [this](std::size_t shard) { ::pthread_rwlock_unlock(&live_index_locks[shard]); },
                [this, &retained](std::size_t shard) {
                    const std::size_t begin = shard * KLiveIndexShardCapacity;
                    const std::size_t end = begin + KLiveIndexShardCapacity;
                    for (std::size_t i = begin; i < end; ++i) {
                        const LiveIndexEntry &entry = live_index[i];
                        void *pointer = entryPointer(entry);
                        LiveAllocation *entry_allocation = entryAllocation(entry);
                        if (pointer != nullptr && pointer != tombstonePointer() && entry_allocation != nullptr &&
                            entryAllocationId(entry) == entry_allocation->allocation_id) {
                            retained.push_back(*entry_allocation);
                        }
                    }
                },
                [] { return std::chrono::steady_clock::now(); }, [] { std::this_thread::yield(); });
        }
        catch (...) {
            lifecycle_readers.fetch_sub(1, std::memory_order_release);
            throw;
        }
        lifecycle_readers.fetch_sub(1, std::memory_order_release);
        if (!stable) {
            error = "timed out stabilizing retained allocation state";
            return false;
        }

        const std::uint64_t captured_ms = monotonicMs();
        std::vector<AllocationProfileAggregation::RetainedSample> prepared;
        prepared.reserve(retained.size());
        for (LiveAllocation &allocation : retained) {
            if (!aggregation.tickAccepts(allocation.tick_id)) {
                continue;
            }
            Sample sample;
            if (!buildSnapshotSample(allocation.frames, allocation.depth, allocation.tick_id, allocation.thread_id,
                                     allocation.os_thread_id, allocation.window, allocation.weight_bytes, sample)) {
                continue;
            }
            prepared.push_back(
                {.sample = std::move(sample),
                 .age_ms = captured_ms >= allocation.allocated_ms ? captured_ms - allocation.allocated_ms : 0});
        }
        return aggregation.buildLiveSnapshot(prepared, snapshot, current_tick.load(std::memory_order_relaxed), error);
    }

    bool setCurrentThreadTrackingSuppressed(bool suppressed) noexcept
    {
        ConsumerGuard registry(registry_consumers);
        if (!registry || backend_shutdown_pending.load(std::memory_order_acquire)) {
            return false;
        }
        ConsumerGuard consumer(*this);
        const bool can_register = consumer && !backend_cleanup_pending.load(std::memory_order_acquire);
        ThreadSamplingState *state = can_register ? currentThreadState() : existingThreadState();
        if (state == nullptr) {
            return false;
        }
        const bool previous = state->tracking_suppressed;
        state->tracking_suppressed = suppressed;
        return previous;
    }

    void markAggregatorFailure(const char *message) noexcept
    {
        markAccountingFailure();
        backend_cleanup_pending.store(true, std::memory_order_release);
        tick_admission_open.store(false, std::memory_order_release);
        tracking.store(false, std::memory_order_release);
        aggregator_running.store(false, std::memory_order_release);
        std::array<char, 256> failure{};
        std::snprintf(failure.data(), failure.size(), "%s",
                      message != nullptr ? message : "unknown allocation aggregator failure");
        for (std::size_t i = 0; i < failure.size(); ++i) {
            aggregator_failure[i].store(failure[i], std::memory_order_relaxed);
        }
        aggregator_failed.store(true, std::memory_order_release);
        stop_requested.store(true, std::memory_order_release);
    }

    std::string aggregatorFailureMessage() const
    {
        std::array<char, 256> failure{};
        for (std::size_t i = 0; i + 1 < failure.size(); ++i) {
            failure[i] = aggregator_failure[i].load(std::memory_order_relaxed);
        }
        return failure.data();
    }

    void resetSession()
    {
        accounting_state.store(AllocationAccountingState::NotStarted, std::memory_order_release);
        TickEvent tick;
        while (ticks.dequeue(tick)) {
        }
        current_tick.store(0, std::memory_order_relaxed);
        for (auto &counters : hot_counters) {
            counters.hook_calls.store(0, std::memory_order_relaxed);
            counters.successful_allocation_calls.store(0, std::memory_order_relaxed);
            counters.observed_bytes.store(0, std::memory_order_relaxed);
        }
        sampling_points.store(0, std::memory_order_relaxed);
        filtered_samples.store(0, std::memory_order_relaxed);
        dropped_samples.store(0, std::memory_order_relaxed);
        dropped_events.store(0, std::memory_order_relaxed);
        dropped_tick_events.store(0, std::memory_order_relaxed);
        enqueued_samples.store(0, std::memory_order_relaxed);
        for (auto &counter : tracking_calls) {
            counter.store(0, std::memory_order_relaxed);
        }
        next_allocation_id.store(1, std::memory_order_relaxed);
        next_session_thread_id.store(1, std::memory_order_relaxed);
        registered_threads.store(0, std::memory_order_relaxed);
        overflow_threads.store(0, std::memory_order_relaxed);
        thread_state_drops.store(0, std::memory_order_relaxed);
        freed_samples.store(0, std::memory_order_relaxed);
        freed_bytes.store(0, std::memory_order_relaxed);
        live_samples.store(0, std::memory_order_relaxed);
        live_bytes.store(0, std::memory_order_relaxed);
        peak_live_samples.store(0, std::memory_order_relaxed);
        lifetime_ms_total.store(0, std::memory_order_relaxed);
        lifetime_ms_max.store(0, std::memory_order_relaxed);
        lifecycle_dropped.store(0, std::memory_order_relaxed);
        contention_dropped.store(0, std::memory_order_relaxed);
        lifecycle_version.store(0, std::memory_order_relaxed);
        lifecycle_readers.store(0, std::memory_order_relaxed);
        lifecycle_writers.store(0, std::memory_order_relaxed);
        drain_truncated.store(0, std::memory_order_relaxed);
        drain_truncated_allocation_events.store(0, std::memory_order_relaxed);
        drain_truncated_thread_observation_events.store(0, std::memory_order_relaxed);
        drain_truncated_tick_events.store(0, std::memory_order_relaxed);
        retained_allocations_skipped.store(0, std::memory_order_relaxed);
        record_pool_acquisition_failures.store(0, std::memory_order_relaxed);
        insertion_contention_failures.store(0, std::memory_order_relaxed);
        exhausted_insertion_probe_failures.store(0, std::memory_order_relaxed);
        detach_contention_attempts.store(0, std::memory_order_relaxed);
        processed_allocation_events.store(0, std::memory_order_relaxed);
        processed_thread_observation_events.store(0, std::memory_order_relaxed);
        processed_tick_events.store(0, std::memory_order_relaxed);
        discarded_allocation_events.store(0, std::memory_order_relaxed);
        discarded_thread_observation_events.store(0, std::memory_order_relaxed);
        discarded_tick_events.store(0, std::memory_order_relaxed);
        consumer_lifetime_elapsed_ns.store(0, std::memory_order_relaxed);
        active_drain_elapsed_ns.store(0, std::memory_order_relaxed);
        caller_final_drain_elapsed_ns.store(0, std::memory_order_relaxed);
        caller_final_drain_allocation_events.store(0, std::memory_order_relaxed);
        caller_final_drain_thread_observation_events.store(0, std::memory_order_relaxed);
        caller_final_drain_tick_events.store(0, std::memory_order_relaxed);
        for (auto &presence : live_presence) {
            presence.store(0, std::memory_order_relaxed);
        }
        deferred_live.store(nullptr, std::memory_order_relaxed);
        tick_admission_open.store(true, std::memory_order_relaxed);
        finalize_pending.store(false, std::memory_order_relaxed);
        pending_finalized.store(false, std::memory_order_relaxed);
        terminal_tick.store(0, std::memory_order_relaxed);
        terminal_ms.store(0, std::memory_order_relaxed);
        terminal_captured.store(false, std::memory_order_relaxed);
        retained_age_ms_total.store(0, std::memory_order_relaxed);
        retained_age_ms_max.store(0, std::memory_order_relaxed);
        for (auto &value : aggregator_failure) {
            value.store('\0', std::memory_order_relaxed);
        }
        aggregator_failed.store(false, std::memory_order_release);
    }

    using Deadline = std::chrono::steady_clock::time_point;

    static Deadline operationDeadline() noexcept { return std::chrono::steady_clock::now() + std::chrono::seconds(5); }

    void setBackendDeadline(Deadline deadline) noexcept
    {
        backend_deadline_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count(),
            std::memory_order_release);
    }

    Deadline backendDeadline() const noexcept
    {
        return Deadline(std::chrono::nanoseconds(backend_deadline_ns.load(std::memory_order_acquire)));
    }

    void disableSession() noexcept
    {
        consumer_state.fetch_or(KConsumersClosed, std::memory_order_acq_rel);
        markStopIncomplete();
        requestFinalization();
        stop_requested.store(true, std::memory_order_release);
        tracking.store(false, std::memory_order_release);
        running.store(false, std::memory_order_release);
        rescan_requested.store(false, std::memory_order_release);
    }

    bool timedOut(std::string &error)
    {
        stop_wait_timed_out.store(true, std::memory_order_release);
        backend_cleanup_pending.store(true, std::memory_order_release);
        disableSession();
        error = "timed out waiting for Linux allocation backend cleanup";
        return false;
    }

    void discardEvents() noexcept
    {
        if (events.storage != nullptr) {
            AllocationEvent event;
            for (std::size_t i = 0; i < KEventCapacity && events.dequeue(event); ++i) {
                if (event.thread_observation) {
                    discarded_thread_observation_events.fetch_add(1, std::memory_order_relaxed);
                    drain_truncated_thread_observation_events.fetch_add(1, std::memory_order_relaxed);
                }
                else {
                    discarded_allocation_events.fetch_add(1, std::memory_order_relaxed);
                    drain_truncated_allocation_events.fetch_add(1, std::memory_order_relaxed);
                }
                drain_truncated.fetch_add(1, std::memory_order_relaxed);
            }
        }
        discardResidualTicks();
    }

    bool waitForConsumers(Deadline deadline, std::string &error, bool registry = false)
    {
        auto &state = registry ? registry_consumers : consumer_state;
        while ((state.load(std::memory_order_acquire) & ~KConsumersClosed) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                error = registry ? "timed out waiting for allocation TLS registry consumers"
                                 : "timed out waiting for allocation snapshot and TLS consumers";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    bool startWorker(const AllocationSamplerConfig &new_config, Deadline deadline, std::string &error)
    {
        consumer_state.fetch_or(KConsumersClosed, std::memory_order_acq_rel);
        if (!waitForConsumers(deadline, error)) {
            return false;
        }
        StartAttemptScope start_attempt(*this);
        resetSession();
        config = new_config;
        aggregation.reset(config, recovery_sink.load(std::memory_order_acquire));
        if (!aggregation.configure(error)) {
            return false;
        }
        interval_bytes.store(static_cast<std::uint64_t>(new_config.interval_bytes), std::memory_order_relaxed);
        last_module_rescan_ms = monotonicMs();
        const std::uint64_t next_generation = generation.fetch_add(1, std::memory_order_relaxed) + 1;
        sampling_seed.store(next_generation ^ monotonicMs() ^ new_config.session_seed, std::memory_order_relaxed);
        session_ready.store(false, std::memory_order_release);
        aggregator_running.store(!config.count_only, std::memory_order_release);
        bool create_aggregator = !config.count_only;
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        create_aggregator = create_aggregator && !fixture_no_worker_for_testing;
        if (linux_control_for_testing != nullptr && linux_control_for_testing->start_failure.load() == 5) {
            error = "injected allocation worker creation failure";
            return false;
        }
        if (start_failure_gate_for_testing != nullptr) {
            start_failure_gate_for_testing->before_thread_creation.store(true, std::memory_order_release);
            while (!start_failure_gate_for_testing->fail_now.load(std::memory_order_acquire) &&
                   !stop_requested.load(std::memory_order_acquire)) {
                yieldLifecycleTest();
            }
            throw AllocationThreadCreationFailureForTesting{};
        }
#endif
        if (create_aggregator &&
            !aggregator_thread.create(
                [](void *opaque) -> void * {
                    auto &impl = *static_cast<Impl *>(opaque);
                    try {
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
                        if (impl.linux_control_for_testing != nullptr &&
                            impl.linux_control_for_testing->aggregator_entry != nullptr) {
                            impl.linux_control_for_testing->aggregator_entry();
                        }
#endif
                        impl.aggregatorLoop();
                    }
                    catch (const std::exception &exception) {
                        impl.markAggregatorFailure(exception.what());
                    }
                    catch (...) {
                        impl.markAggregatorFailure("allocation aggregator failed with an unknown exception");
                    }
                    return nullptr;
                },
                this)) {
            error = "could not create the allocation aggregator thread";
            return false;
        }
        if (!create_aggregator) {
            aggregator_running.store(false, std::memory_order_release);
        }
        if (stop_requested.load(std::memory_order_acquire) || !prepareHooks(error)) {
            return false;
        }
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        if (linux_control_for_testing != nullptr && linux_control_for_testing->start_failure.load() == 4) {
            error = "injected allocation storage failure";
            return false;
        }
#endif
        if (!new_config.count_only && (!events.allocate(error) || !allocateLifecycleStorage(error))) {
            return false;
        }
        if (stop_requested.load(std::memory_order_acquire) || !installHooks(error)) {
            return false;
        }
        accounting_state.store(config.count_only ? AllocationAccountingState::NotApplicable
                                                 : AllocationAccountingState::Active,
                               std::memory_order_release);
        consumer_state.fetch_and(~KConsumersClosed, std::memory_order_release);
        registry_consumers.fetch_and(~KConsumersClosed, std::memory_order_release);
        running.store(true, std::memory_order_release);
        tracking.store(true, std::memory_order_release);
        session_ready.store(true, std::memory_order_release);
        if (stop_requested.load(std::memory_order_acquire)) {
            disableSession();
            return false;
        }
        failed_start_pending.store(false, std::memory_order_release);
        start_attempt.dismiss();
        return true;
    }

    bool cleanupWorker(Deadline deadline, bool final, std::string &error)
    {
        if (final) {
            registry_consumers.fetch_or(KConsumersClosed, std::memory_order_acq_rel);
        }
        disableSession();
        gateway.close(final);
        if (!gateway.waitUntil(deadline, final, error) ||
            !waitFor(tracking_calls, "tracked Linux allocation hooks", error, deadline) ||
            !waitForConsumers(deadline, error) || (final && !waitForConsumers(deadline, error, true))) {
            return false;
        }
        while (tick_calls.load(std::memory_order_acquire) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                error = "timed out waiting for allocation tick accounting";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!terminal_captured.exchange(true, std::memory_order_acq_rel)) {
            terminal_tick.store(current_tick.load(std::memory_order_relaxed), std::memory_order_release);
        }
        aggregator_running.store(false, std::memory_order_release);
        if (!aggregator_thread.joinUntil(deadline)) {
            error = "timed out waiting for allocation aggregator thread exit";
            return false;
        }
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
        if (fixture_no_worker_for_testing && !failed_start_pending.load(std::memory_order_acquire)) {
            finishAggregationIfNeeded();
            if (config.live_only && live_index != nullptr) {
                finalizeLiveProfile();
            }
            publishComplete();
        }
#endif
        discardEvents();
        if (pending_finalized.load(std::memory_order_acquire) && !aggregator_failed.load(std::memory_order_acquire)) {
            publishComplete();
        }
        if (final) {
            if (hooks.installed() && !hooks.uninstall(error)) {
                publishHookDiagnostics();
                return false;
            }
            publishHookDiagnostics();
            if (gateway.reserved() && !gateway.retired()) {
                if (!gateway_published) {
                    if (!gateway.cancel()) {
                        error = "cannot cancel unpublished Linux allocation gateway";
                        return false;
                    }
                }
                else if (!releaseThreadStateRegistry(error) || !gateway.retire(error)) {
                    return false;
                }
            }
            Impl *expected = this;
            mActiveInstance.compare_exchange_strong(expected, nullptr, std::memory_order_release,
                                                    std::memory_order_relaxed);
            failed_start_pending.store(false, std::memory_order_release);
            gateway_published = false;
        }
        else if (!gateway.clear(false, error)) {
            return false;
        }
        events.release();
        releaseLifecycleStorage();
        if (!final) {
            consumer_state.fetch_and(~KConsumersClosed, std::memory_order_release);
        }
        return true;
    }

    void completeBackend(std::uint64_t revision, bool success, const std::string &error) noexcept
    {
        try {
            delete backend_message.exchange(new std::string(error), std::memory_order_acq_rel);
        }
        catch (...) {
            delete backend_message.exchange(nullptr, std::memory_order_acq_rel);
        }
        backend_success.store(success, std::memory_order_release);
        backend_completed_revision.store(revision, std::memory_order_release);
    }

    void backendLoop() noexcept
    {
        std::uint64_t observed = 0;
        for (;;) {
            const auto revision = backend_revision.load(std::memory_order_acquire);
            if (revision != observed) {
                observed = revision;
                const auto deadline = backendDeadline();
                bool started = false;
                bool success = false;
                std::string error;
                const bool start_attempt = backend_state.load(std::memory_order_acquire) == BackendState::Starting;
                if (start_attempt && !stop_requested.load(std::memory_order_acquire)) {
                    try {
                        started = startWorker(pending_config, deadline, error);
                    }
                    catch (const std::exception &exception) {
                        error = exception.what();
                    }
                    catch (...) {
                        error = "allocation backend start failed";
                    }
                    if (started) {
                        backend_state.store(BackendState::Active, std::memory_order_release);
                        completeBackend(revision, true, error);
                        continue;
                    }
                    failed_start_pending.store(true, std::memory_order_release);
                    markAccountingFailure();
                }
                const bool failed_start = failed_start_pending.load(std::memory_order_acquire);
                const bool final = failed_start || backend_shutdown_pending.load(std::memory_order_acquire);
                backend_state.store(BackendState::Cleaning, std::memory_order_release);
                try {
                    std::string cleanup_error;
                    success = cleanupWorker(deadline, final, cleanup_error);
                    if (!cleanup_error.empty()) {
                        error = std::move(cleanup_error);
                    }
                }
                catch (const std::exception &exception) {
                    error = exception.what();
                }
                catch (...) {
                    error = "allocation backend cleanup failed";
                }
                const bool exit = success && backend_shutdown_pending.load(std::memory_order_acquire) && final;
                auto state = success ? BackendState::Idle : BackendState::Failed;
                if (exit) {
                    state = BackendState::Exited;
                }
                backend_state.store(state, std::memory_order_release);
                completeBackend(revision, success && !start_attempt, error);
                if (exit) {
                    return;
                }
            }
            else if (backend_state.load(std::memory_order_acquire) == BackendState::Active) {
                if (stop_requested.load(std::memory_order_acquire)) {
                    setBackendDeadline(operationDeadline());
                    backend_revision.fetch_add(1, std::memory_order_acq_rel);
                    continue;
                }
                if (rescan_requested.exchange(false, std::memory_order_acq_rel)) {
                    rescan_active.store(true, std::memory_order_release);
                    try {
                        std::string ignored;
                        hooks.rescan(ignored);
                        updateHookCapabilities();
                    }
                    catch (...) {
                        failed_module_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    rescan_active.store(false, std::memory_order_release);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool awaitBackend(std::uint64_t revision, Deadline deadline, std::string &error)
    {
        while (backend_completed_revision.load(std::memory_order_acquire) < revision) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return timedOut(error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const std::unique_ptr<std::string> message(backend_message.exchange(nullptr, std::memory_order_acq_rel));
        error = message != nullptr ? *message : "allocation backend failed";
        return backend_success.load(std::memory_order_acquire);
    }

    bool startSession(const AllocationSamplerConfig &new_config, std::string &error)
    {
        const auto deadline = operationDeadline();
        std::unique_lock lock(lifecycle_mutex, std::defer_lock);
        if (!lock.try_lock_until(deadline)) {
            return timedOut(error);
        }
        error.clear();
        if (running.load(std::memory_order_acquire) || backendCleanupPending() ||
            backend_state.load(std::memory_order_acquire) != BackendState::Idle) {
            error = "the previous allocation session has not finished cleanup";
            return false;
        }
        if (new_config.session_seed == 0 || new_config.interval_bytes <= 0) {
            error = "invalid Linux allocation sampler configuration";
            return false;
        }
        pending_config = new_config;
        stop_requested.store(false, std::memory_order_release);
        stop_wait_timed_out.store(false, std::memory_order_release);
        failed_start_pending.store(true, std::memory_order_release);
        backend_cleanup_pending.store(true, std::memory_order_release);
        backend_state.store(BackendState::Starting, std::memory_order_release);
        setBackendDeadline(deadline);
        const auto revision = backend_revision.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (!backend_thread.joinable() && !backend_thread.create(
                                              [](void *opaque) -> void * {
                                                  static_cast<Impl *>(opaque)->backendLoop();
                                                  return nullptr;
                                              },
                                              this)) {
            failed_start_pending.store(false, std::memory_order_release);
            backend_cleanup_pending.store(false, std::memory_order_release);
            backend_state.store(BackendState::Idle, std::memory_order_release);
            markAccountingFailure();
            error = "could not create the allocation backend thread";
            return false;
        }
        const bool success = awaitBackend(revision, deadline, error);
        if (success) {
            std::unique_lock tick_lock(tick_mutex, std::defer_lock);
            if (!tick_lock.try_lock_until(deadline)) {
                return timedOut(error);
            }
            consumeHookCapabilities();
            backend_cleanup_pending.store(false, std::memory_order_release);
        }
        else if (backend_state.load(std::memory_order_acquire) == BackendState::Idle &&
                 !failed_start_pending.load(std::memory_order_acquire)) {
            backend_cleanup_pending.store(false, std::memory_order_release);
        }
        return success;
    }

    bool finishSession(bool shutdown, std::string &error)
    {
        const auto deadline = operationDeadline();
        if (shutdown) {
            backend_shutdown_pending.store(true, std::memory_order_release);
            registry_consumers.fetch_or(KConsumersClosed, std::memory_order_acq_rel);
        }
        disableSession();
        std::unique_lock lock(lifecycle_mutex, std::defer_lock);
        if (!lock.try_lock_until(deadline)) {
            return timedOut(error);
        }
        error.clear();
        if (!backend_thread.joinable()) {
            backend_shutdown_pending.store(false, std::memory_order_release);
            backend_cleanup_pending.store(false, std::memory_order_release);
            return true;
        }
        if (!shutdown && backend_shutdown_pending.load(std::memory_order_acquire)) {
            error = "allocation backend shutdown cleanup is pending";
            return false;
        }
        backend_cleanup_pending.store(true, std::memory_order_release);
        bool success = true;
        if (backend_state.load(std::memory_order_acquire) != BackendState::Exited) {
            setBackendDeadline(deadline);
            const auto revision = backend_revision.fetch_add(1, std::memory_order_acq_rel) + 1;
            success = awaitBackend(revision, deadline, error);
        }
        if (shutdown && backend_state.load(std::memory_order_acquire) == BackendState::Exited) {
            if (!backend_thread.joinUntil(deadline)) {
                return timedOut(error);
            }
            backend_shutdown_pending.store(false, std::memory_order_release);
            backend_state.store(BackendState::Idle, std::memory_order_release);
            success = true;
        }
        if (!success) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return timedOut(error);
            }
            return false;
        }
        if (backend_shutdown_pending.load(std::memory_order_acquire)) {
            error = "allocation backend shutdown cleanup is pending";
            return false;
        }
        backend_cleanup_pending.store(false, std::memory_order_release);
        if (shutdown) {
            return true;
        }
        if (aggregator_failed.load(std::memory_order_acquire)) {
            error = "allocation aggregator failed: " + aggregatorFailureMessage();
            return false;
        }
        if (config.live_only && lifecycle_dropped.load(std::memory_order_relaxed) != 0) {
            error = "allocation lifecycle tracking lost records; retained profile discarded";
            return false;
        }
        return true;
    }

    bool stopSession(std::string &error) { return finishSession(false, error); }
    bool shutdownBackend(std::string &error) { return finishSession(true, error); }

    bool releaseThreadStateRegistry(std::string &error)
    {
        if (thread_state_key_created) {
            int result = 0;
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
            if (linux_control_for_testing != nullptr && linux_control_for_testing->key_delete_failure.load()) {
                result = EINVAL;
            }
            else
#endif
            {
                result = ::pthread_key_delete(thread_state_key);
            }
            if (result != 0) {
                error = "pthread_key_delete for allocation thread state failed: " + std::string(std::strerror(result));
                return false;
            }
            thread_state_key_created = false;
        }
        if (!gateway.clear(false, error) || !gateway.clear(true, error)) {
            return false;
        }
        for (ThreadSamplingState &state : thread_states) {
            state.owner_tid.store(0, std::memory_order_relaxed);
            state.registry_state.store(0, std::memory_order_relaxed);
        }
        return true;
    }

    void tick(double mspt_ms)
    {
        tick_calls.fetch_add(1, std::memory_order_acq_rel);
        struct TickScope {
            std::atomic<std::uint64_t> &calls;
            ~TickScope() { calls.fetch_sub(1, std::memory_order_release); }
        } scope{tick_calls};
        std::scoped_lock tick_lock(tick_mutex);
        if (stop_requested.load(std::memory_order_acquire) || !tick_admission_open.load(std::memory_order_acquire) ||
            !running.load(std::memory_order_acquire) || aggregator_failed.load(std::memory_order_acquire)) {
            return;
        }
        TrackingSuppressionGuard suppress(*this);
        const std::uint64_t finished = current_tick.fetch_add(1, std::memory_order_relaxed);
        if (config.only_ticks_over_ms > 0 && !ticks.enqueue(TickEvent{.tick_id = finished, .mspt_ms = mspt_ms})) {
            dropped_tick_events.fetch_add(1, std::memory_order_relaxed);
        }
        const std::uint64_t now = monotonicMs();
        if (now >= last_module_rescan_ms + 5000) {
            rescan_requested.store(true, std::memory_order_release);
            last_module_rescan_ms = now;
        }
        consumeHookCapabilities();
        if (config.count_only) {
            return;
        }
        const std::int32_t window = profiling_window::windowNow();
        aggregation.recordTick(window, mspt_ms);
    }
};

std::atomic<AllocationSampler::Impl *> AllocationSampler::Impl::mActiveInstance{nullptr};

#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
namespace test {

bool AllocationLifecycleTestAccess::configureLinux(AllocationSampler &sampler,
                                                   LinuxAllocationTestControl *control) noexcept
{
    if (sampler.impl_->running.load() || sampler.impl_->backendCleanupPending()) {
        return false;
    }
    sampler.impl_->linux_control_for_testing = control;
    return true;
}

void AllocationLifecycleTestAccess::requestLinuxRescan(AllocationSampler &sampler) noexcept
{
    sampler.impl_->rescan_requested.store(true, std::memory_order_release);
}

std::uint32_t AllocationLifecycleTestAccess::linuxGroup(const AllocationSampler &sampler) noexcept
{
    return sampler.impl_->gateway.binding().group;
}

bool AllocationLifecycleTestAccess::linuxKeyCreated(const AllocationSampler &sampler) noexcept
{
    return sampler.impl_->thread_state_key_created;
}

bool AllocationLifecycleTestAccess::linuxRescanActive(const AllocationSampler &sampler) noexcept
{
    return sampler.impl_->rescan_active.load(std::memory_order_acquire);
}

bool AllocationDiagnosticsTestAccess::configureFixture(AllocationSampler &sampler, bool no_hooks, bool no_worker,
                                                       AllocationFixtureWorkerGate *worker_gate) noexcept
{
    if ((no_worker && worker_gate != nullptr) || (!no_worker && worker_gate == nullptr)) {
        return false;
    }
    if (sampler.impl_->running.load(std::memory_order_acquire) || sampler.impl_->backendCleanupPending() ||
        sampler.impl_->hooks.installed() || sampler.impl_->events.storage != nullptr ||
        sampler.impl_->live_storage != nullptr || sampler.impl_->live_index != nullptr ||
        sampler.impl_->fixture_controls_configured_for_testing ||
        sampler.impl_->fixture_cpu_work_for_testing != nullptr) {
        return false;
    }
    sampler.impl_->fixture_no_hooks_for_testing = no_hooks;
    sampler.impl_->fixture_no_worker_for_testing = no_worker;
    sampler.impl_->fixture_controls_configured_for_testing = true;
    sampler.impl_->fixture_worker_gate_for_testing = worker_gate;
    sampler.impl_->fixture_thread_state_active_for_testing = no_hooks && no_worker;
    const auto fixture_owner_tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
    sampler.impl_->fixture_thread_owner_for_testing =
        sampler.impl_->fixture_thread_state_active_for_testing ? fixture_owner_tid : 0;
    if (worker_gate != nullptr) {
        worker_gate->entered.store(false, std::memory_order_relaxed);
        worker_gate->release.store(false, std::memory_order_relaxed);
        worker_gate->timed_out.store(false, std::memory_order_relaxed);
    }
    sampler.impl_->fixture_thread_state_for_testing.registry_state.store(0, std::memory_order_relaxed);
    sampler.impl_->fixture_thread_state_for_testing.owner_tid.store(
        sampler.impl_->fixture_thread_state_active_for_testing ? fixture_owner_tid : 0, std::memory_order_relaxed);
    sampler.impl_->fixture_thread_state_for_testing.bytes = {};
    sampler.impl_->fixture_thread_state_for_testing.identity_generation = 0;
    sampler.impl_->fixture_thread_state_for_testing.session_thread_id = 0;
    sampler.impl_->fixture_thread_state_for_testing.os_thread_id = 0;
    sampler.impl_->fixture_thread_state_for_testing.inside_hook = false;
    sampler.impl_->fixture_thread_state_for_testing.tracking_suppressed = false;
    sampler.impl_->fixture_thread_state_for_testing.identity_announced = false;
    return true;
}

bool AllocationDiagnosticsTestAccess::releaseFixture(AllocationSampler &sampler) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        sampler.impl_->running.load(std::memory_order_acquire) || sampler.impl_->backendCleanupPending() ||
        sampler.impl_->hooks.installed() || sampler.impl_->events.storage != nullptr ||
        sampler.impl_->live_storage != nullptr || sampler.impl_->live_index != nullptr ||
        sampler.impl_->aggregator_thread.joinable()) {
        return false;
    }
    sampler.impl_->fixture_worker_gate_for_testing = nullptr;
    sampler.impl_->fixture_cpu_work_for_testing = nullptr;
    sampler.impl_->start_failure_gate_for_testing = nullptr;
    sampler.impl_->force_process_event_failure_for_testing = false;
    sampler.impl_->fixture_no_hooks_for_testing = false;
    sampler.impl_->fixture_no_worker_for_testing = false;
    sampler.impl_->fixture_controls_configured_for_testing = false;
    sampler.impl_->fixture_thread_owner_for_testing = 0;
    sampler.impl_->fixture_thread_state_active_for_testing = false;
    return true;
}

bool AllocationDiagnosticsTestAccess::fixtureStorageReady(const AllocationSampler &sampler) noexcept
{
    return sampler.impl_->events.storage != nullptr && sampler.impl_->live_storage != nullptr &&
           sampler.impl_->live_index != nullptr;
}

bool AllocationDiagnosticsTestAccess::fixtureWorkerPresent(const AllocationSampler &sampler) noexcept
{
    return sampler.impl_->aggregator_thread.joinable();
}

bool AllocationDiagnosticsTestAccess::fixtureHooksPresent(const AllocationSampler &sampler) noexcept
{
    return sampler.impl_->hooks.installed();
}

bool AllocationDiagnosticsTestAccess::fixtureAggregatorRunning(const AllocationSampler &sampler) noexcept
{
    return sampler.impl_->aggregator_running.load(std::memory_order_acquire);
}

bool AllocationDiagnosticsTestAccess::seedFixtureQueues(AllocationSampler &sampler,
                                                        AllocationFixtureSeedCounts &result) noexcept
{
    return seedFixtureQueues(
        sampler, result,
        AllocationFixtureSeedCounts{.allocation_events = 3, .thread_observation_events = 2, .tick_events = 5});
}

bool AllocationDiagnosticsTestAccess::seedFixtureQueues(AllocationSampler &sampler, AllocationFixtureSeedCounts &result,
                                                        const AllocationFixtureSeedCounts &requested) noexcept
{
    result = {};
    const bool accepted_request =
        (requested.allocation_events == 3 && requested.thread_observation_events == 2 &&
         (requested.tick_events == 5 || requested.tick_events == 0)) ||
        (requested.allocation_events == 1 && requested.thread_observation_events == 0 && requested.tick_events == 0);
    if (!accepted_request) {
        return false;
    }

    try {
        std::unique_lock lifecycle_lock(sampler.impl_->lifecycle_mutex);
        AllocationFixtureWorkerGate *gate = sampler.impl_->fixture_worker_gate_for_testing;
        std::unique_lock<std::mutex> seed_lock;
        if (gate != nullptr) {
            seed_lock = std::unique_lock<std::mutex>(gate->seed_mutex);
        }
        const auto admitted = [&] {
            if (!sampler.impl_->fixture_controls_configured_for_testing ||
                !sampler.impl_->running.load(std::memory_order_acquire) ||
                sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
                !sampler.impl_->fixture_no_hooks_for_testing || !fixtureStorageReady(sampler) ||
                !sampler.impl_->tick_admission_open.load(std::memory_order_acquire)) {
                return false;
            }
            if (sampler.impl_->fixture_no_worker_for_testing) {
                return gate == nullptr;
            }
            return gate != nullptr && gate->entered.load(std::memory_order_acquire) &&
                   !gate->release.load(std::memory_order_acquire) && !gate->timed_out.load(std::memory_order_acquire);
        };
        if (!admitted()) {
            return false;
        }

        const auto event_count =
            static_cast<std::size_t>(requested.allocation_events + requested.thread_observation_events);
        std::array<AllocationSampler::Impl::AllocationEvent, 5> events{};
        for (std::size_t i = 0; i < event_count; ++i) {
            events[i].weight_bytes = 1;
            events[i].tick_id = 0;
            events[i].thread_id = 100 + i;
            events[i].os_thread_id = 200 + i;
            events[i].window = 0;
            events[i].thread_observation = i >= requested.allocation_events;
            if (!events[i].thread_observation) {
                events[i].depth = 1;
                events[i].frames[0] = reinterpret_cast<cpptrace::frame_ptr>(&diagnosticsTestFrameAnchor);
            }
        }

        for (std::size_t i = 0; i < event_count; ++i) {
            if (!admitted() || !sampler.impl_->events.enqueue(events[i])) {
                return false;
            }
            if (events[i].thread_observation) {
                ++result.thread_observation_events;
            }
            else {
                ++result.allocation_events;
            }
        }
        for (std::size_t i = 0; i < requested.tick_events; ++i) {
            if (!admitted() ||
                !sampler.impl_->ticks.enqueue(AllocationSampler::Impl::TickEvent{.tick_id = i, .mspt_ms = 1.0})) {
                return false;
            }
            ++result.tick_events;
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

bool AllocationDiagnosticsTestAccess::recordFixtureAllocation(AllocationSampler &sampler, void *pointer,
                                                              std::uint64_t requested_bytes) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        pointer == nullptr || requested_bytes == 0 || !sampler.impl_->fixture_no_hooks_for_testing ||
        !sampler.impl_->fixture_no_worker_for_testing ||
        !sampler.impl_->tick_admission_open.load(std::memory_order_acquire) || !fixtureStorageReady(sampler)) {
        return false;
    }
    sampler.impl_->recordAllocation(pointer, requested_bytes);
    return true;
}

bool AllocationLifecycleTestAccess::holdTrackingCall(AllocationSampler &sampler, TrackingGate &gate) noexcept
{
    const auto *start_gate = sampler.impl_->start_failure_gate_for_testing;
    if (start_gate != nullptr && start_gate->before_thread_creation.load(std::memory_order_acquire)) {
        auto &counter = sampler.impl_->tracking_calls[AllocationSampler::Impl::currentHookShard()];
        counter.fetch_add(1, std::memory_order_acq_rel);
        gate.entered.store(true, std::memory_order_release);
        while (!gate.release.load(std::memory_order_acquire)) {
            yieldLifecycleTest();
        }
        counter.fetch_sub(1, std::memory_order_release);
        gate.exited.store(true, std::memory_order_release);
        return true;
    }
    bool admitted = false;
    {
        AllocationSampler::Impl::TrackingCallGuard tracking_guard(*sampler.impl_);
        if (tracking_guard) {
            gate.entered.store(true, std::memory_order_release);
            while (!gate.release.load(std::memory_order_acquire)) {
                yieldLifecycleTest();
            }
            admitted = true;
        }
    }
    gate.exited.store(true, std::memory_order_release);
    return admitted;
}

void AllocationLifecycleTestAccess::armThreadCreationFailure(AllocationSampler &sampler,
                                                             StartFailureGate &gate) noexcept
{
    sampler.impl_->start_failure_gate_for_testing = &gate;
}

void AllocationLifecycleTestAccess::disarmThreadCreationFailure(AllocationSampler &sampler) noexcept
{
    sampler.impl_->start_failure_gate_for_testing = nullptr;
}

void AllocationDiagnosticsTestAccess::forceAggregatorCpuReadFailure(AllocationSampler &, bool) noexcept {}

void AllocationDiagnosticsTestAccess::forceAggregatorCpuZero(AllocationSampler &, bool) noexcept {}

void AllocationDiagnosticsTestAccess::armEventProcessingGate(AllocationSampler &,
                                                             AllocationEventProcessingGate &gate) noexcept
{
    gate.entered.store(false, std::memory_order_relaxed);
    gate.release.store(false, std::memory_order_relaxed);
    gate.timed_out.store(false, std::memory_order_relaxed);
}

void AllocationDiagnosticsTestAccess::disarmEventProcessingGate(AllocationSampler &) noexcept {}

void AllocationDiagnosticsTestAccess::forceProcessEventFailure(AllocationSampler &sampler, bool force) noexcept
{
    sampler.impl_->force_process_event_failure_for_testing = force;
}

bool AllocationDiagnosticsTestAccess::configureFixtureCpuWork(AllocationSampler &,
                                                              AllocationFixtureCpuWorkControl &) noexcept
{
    return false;
}

void AllocationDiagnosticsTestAccess::forceDrainDeadline(AllocationSampler &sampler, bool force) noexcept
{
    (void)sampler;
    (void)force;
}

void AllocationDiagnosticsTestAccess::forceRetainedWalkBudget(AllocationSampler &sampler, bool force) noexcept
{
    (void)sampler;
    (void)force;
}

std::uint64_t AllocationDiagnosticsTestAccess::retainedWalkVisits(const AllocationSampler &) noexcept
{
    return 0;
}

bool AllocationDiagnosticsTestAccess::drainFixtureAggregatorContext(AllocationSampler &sampler) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->events.storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr) {
        return false;
    }
    std::unique_lock lock(sampler.impl_->aggregate_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(5))) {
        return false;
    }
    sampler.impl_->drainQueues(AllocationSampler::Impl::DrainContext::Aggregator);
    return true;
}

bool AllocationDiagnosticsTestAccess::seedFixtureLiveAllocations(AllocationSampler &sampler, void *const *pointers,
                                                                 std::size_t count) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->events.storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr ||
        (count != 0 && pointers == nullptr)) {
        return false;
    }
    std::vector<AllocationSampler::Impl::LiveAllocation *> inserted;
    try {
        inserted.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (pointers[i] == nullptr) {
                throw std::runtime_error("null fixture live pointer");
            }
            auto *record = sampler.impl_->acquireLiveRecord();
            if (record == nullptr) {
                throw std::runtime_error("fixture live record pool exhausted");
            }
            *record = AllocationSampler::Impl::LiveAllocation{};
            record->pointer = pointers[i];
            record->allocation_id = sampler.impl_->next_allocation_id.fetch_add(1, std::memory_order_relaxed);
            record->weight_bytes = 1;
            record->requested_bytes = 1;
            record->allocated_ms = monotonicMs();
            record->tick_id = 0;
            record->thread_id = 1;
            record->os_thread_id = 1;
            record->window = 0;
            record->depth = 1;
            record->frames[0] = reinterpret_cast<cpptrace::frame_ptr>(&diagnosticsTestFrameAnchor);
            if (!sampler.impl_->insertLiveAllocation(record, true)) {
                sampler.impl_->recycleLiveRecord(record);
                throw std::runtime_error("fixture live insertion failed");
            }
            inserted.push_back(record);
        }
        return true;
    }
    catch (...) {
        for (auto *record : inserted) {
            AllocationSampler::Impl::LiveAllocation *detached = sampler.impl_->detachAllocation(record->pointer);
            if (detached != nullptr) {
                sampler.impl_->retireAllocation(detached, monotonicMs());
            }
        }
        return false;
    }
}

bool AllocationDiagnosticsTestAccess::prepareFixtureLiveRecord(AllocationSampler &sampler, void *pointer,
                                                               std::uint64_t requested_bytes,
                                                               AllocationFixtureLiveRecord &result) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->events.storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr || pointer == nullptr ||
        requested_bytes == 0 || result.opaque != nullptr) {
        return false;
    }
    auto *record = sampler.impl_->acquireLiveRecord();
    if (record == nullptr) {
        return false;
    }
    *record = AllocationSampler::Impl::LiveAllocation{};
    record->pointer = pointer;
    record->allocation_id = sampler.impl_->next_allocation_id.fetch_add(1, std::memory_order_relaxed);
    record->weight_bytes = requested_bytes;
    record->requested_bytes = requested_bytes;
    record->allocated_ms = monotonicMs();
    record->tick_id = 0;
    record->thread_id = 1;
    record->os_thread_id = 1;
    record->window = 0;
    record->depth = 1;
    record->frames[0] = reinterpret_cast<cpptrace::frame_ptr>(&diagnosticsTestFrameAnchor);
    result.pointer = pointer;
    result.opaque = record;
    return true;
}

bool AllocationDiagnosticsTestAccess::holdInsertionShardLock(AllocationSampler &sampler, void *pointer,
                                                             AllocationFixtureLockGate &gate) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->live_index == nullptr || pointer == nullptr) {
        return false;
    }
    const std::size_t shard = AllocationSampler::Impl::liveIndexShard(AllocationSampler::Impl::liveIndexHash(pointer));
    if (::pthread_rwlock_wrlock(&sampler.impl_->live_index_locks[shard]) != 0) {
        return false;
    }
    gate.ready.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!gate.release.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            gate.timed_out.store(true, std::memory_order_release);
            break;
        }
        yieldLifecycleTest();
    }
    ::pthread_rwlock_unlock(&sampler.impl_->live_index_locks[shard]);
    return !gate.timed_out.load(std::memory_order_acquire);
}

bool AllocationDiagnosticsTestAccess::holdDetachShardLock(AllocationSampler &sampler, void *pointer,
                                                          AllocationFixtureLockGate &gate) noexcept
{
    return holdInsertionShardLock(sampler, pointer, gate);
}

bool AllocationDiagnosticsTestAccess::insertFixtureLiveRecord(AllocationSampler &sampler,
                                                              AllocationFixtureLiveRecord &record,
                                                              bool account_live) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->events.storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr || record.opaque == nullptr ||
        record.pointer == nullptr) {
        return false;
    }
    auto *allocation = static_cast<AllocationSampler::Impl::LiveAllocation *>(record.opaque);
    if (!sampler.impl_->insertLiveAllocation(allocation, account_live)) {
        return false;
    }
    record.opaque = nullptr;
    return true;
}

bool AllocationDiagnosticsTestAccess::detachFixtureLiveRecord(AllocationSampler &sampler,
                                                              AllocationFixtureLiveRecord &record) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->events.storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr || record.pointer == nullptr ||
        record.opaque != nullptr) {
        return false;
    }
    AllocationSampler::Impl::LiveAllocation *allocation = sampler.impl_->detachAllocation(record.pointer);
    if (allocation == nullptr) {
        return false;
    }
    record.opaque = allocation;
    return true;
}

void AllocationDiagnosticsTestAccess::retireFixtureLiveRecord(AllocationSampler &sampler,
                                                              AllocationFixtureLiveRecord &record) noexcept
{
    if (record.opaque == nullptr) {
        return;
    }
    auto *allocation = static_cast<AllocationSampler::Impl::LiveAllocation *>(record.opaque);
    sampler.impl_->retireAllocation(allocation, monotonicMs());
    record.opaque = nullptr;
}

void AllocationDiagnosticsTestAccess::releaseFixtureLiveRecord(AllocationSampler &sampler,
                                                               AllocationFixtureLiveRecord &record) noexcept
{
    if (record.opaque == nullptr) {
        return;
    }
    sampler.impl_->recycleLiveRecord(static_cast<AllocationSampler::Impl::LiveAllocation *>(record.opaque));
    record.opaque = nullptr;
}

bool AllocationDiagnosticsTestAccess::fileTimeToNanoseconds(std::uint32_t, std::uint32_t, std::uint64_t &value) noexcept
{
    value = 0;
    return false;
}

void AllocationDiagnosticsTestAccess::seedModuleCache(AllocationSampler &, std::size_t) noexcept {}

bool AllocationDiagnosticsTestAccess::resolveFrameOnce(AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::resolveFrameTwice(AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::resolveMissingFrame(AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::exerciseRecordPoolEmpty(AllocationSampler &sampler, void *pointer) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->events.storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr || pointer == nullptr) {
        return false;
    }
    if (::pthread_mutex_lock(&sampler.impl_->live_pool_mutex) != 0) {
        return false;
    }
    AllocationSampler::Impl::LiveAllocation *held_free = sampler.impl_->free_live;
    AllocationSampler::Impl::LiveAllocation *held_deferred =
        sampler.impl_->deferred_live.exchange(nullptr, std::memory_order_acq_rel);
    sampler.impl_->free_live = nullptr;
    ::pthread_mutex_unlock(&sampler.impl_->live_pool_mutex);

    sampler.impl_->recordAllocation(pointer, 1);

    if (::pthread_mutex_lock(&sampler.impl_->live_pool_mutex) != 0) {
        sampler.impl_->free_live = held_free;
        sampler.impl_->deferred_live.store(held_deferred, std::memory_order_release);
        return false;
    }
    sampler.impl_->free_live = held_free;
    sampler.impl_->deferred_live.store(held_deferred, std::memory_order_release);
    ::pthread_mutex_unlock(&sampler.impl_->live_pool_mutex);
    return true;
}

bool AllocationDiagnosticsTestAccess::exerciseInsertionProbeExhaustion(AllocationSampler &sampler, void *backing,
                                                                       std::size_t backing_bytes) noexcept
{
    constexpr std::size_t fixture_count = KLiveIndexShardCapacity + 1;
    constexpr std::size_t pointer_quantum = sizeof(std::uint64_t) * 2;
    constexpr std::size_t pointer_stride = KLiveIndexShards * pointer_quantum;
    static_assert(pointer_quantum == 16);

    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->events.storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr || backing == nullptr ||
        backing_bytes < fixture_count * pointer_stride) {
        return false;
    }

    try {
        std::array<AllocationSampler::Impl::LiveAllocation *, fixture_count> records{};
        std::size_t successful = 0;
        bool final_failed = false;
        for (std::size_t i = 0; i < fixture_count; ++i) {
            AllocationSampler::Impl::LiveAllocation *record = sampler.impl_->acquireLiveRecord();
            if (record == nullptr) {
                break;
            }
            record->pointer = static_cast<void *>(static_cast<unsigned char *>(backing) + i * pointer_stride);
            record->allocation_id = sampler.impl_->next_allocation_id.fetch_add(1, std::memory_order_relaxed);
            record->weight_bytes = 1;
            record->requested_bytes = 1;
            record->allocated_ms = monotonicMs();
            record->tick_id = 0;
            record->thread_id = 0;
            record->os_thread_id = 0;
            record->window = 0;
            record->depth = 1;
            record->frames[0] = reinterpret_cast<cpptrace::frame_ptr>(&diagnosticsTestFrameAnchor);
            records[i] = record;
            if (!sampler.impl_->insertLiveAllocation(record, true)) {
                final_failed = i == KLiveIndexShardCapacity;
                sampler.impl_->recycleLiveRecord(record);
                records[i] = nullptr;
                break;
            }
            ++successful;
        }

        bool cleanup_ok = true;
        for (std::size_t i = 0; i < successful; ++i) {
            AllocationSampler::Impl::LiveAllocation *detached = sampler.impl_->detachAllocation(records[i]->pointer);
            if (detached == nullptr) {
                cleanup_ok = false;
                continue;
            }
            sampler.impl_->retireAllocation(detached, monotonicMs());
            records[i] = nullptr;
        }
        return cleanup_ok && successful == KLiveIndexShardCapacity && final_failed;
    }
    catch (...) {
        return false;
    }
}

}  // namespace test
#endif

AllocationSampler::AllocationSampler() : impl_(std::make_unique<Impl>()) {}

AllocationSampler::~AllocationSampler()
{
    if (impl_ == nullptr) {
        return;
    }
    std::string error;
    if (impl_->shutdownBackend(error)) {
        return;
    }
    if (!error.empty()) {
        std::fprintf(stderr, "[spark] Linux allocation sampler shutdown failed: %s\n", error.c_str());
    }
    std::abort();
}

bool AllocationSampler::start(const AllocationSamplerConfig &config, std::string &error)
{
    return impl_->startSession(config, error);
}

void AllocationSampler::setRecoverySink(RecoverySink *sink)
{
    impl_->recovery_sink.store(sink, std::memory_order_release);
    impl_->aggregation.setRecoverySink(sink);
}

bool AllocationSampler::stop(std::string &error)
{
    return impl_->stopSession(error);
}

void AllocationSampler::requestStop() noexcept
{
    const bool active = impl_->running.load(std::memory_order_acquire) ||
                        impl_->backend_cleanup_pending.load(std::memory_order_acquire);
    if (active) {
        impl_->backend_cleanup_pending.store(true, std::memory_order_release);
        impl_->disableSession();
    }
}

bool AllocationSampler::shutdown(std::string &error)
{
    return impl_->shutdownBackend(error);
}
void AllocationSampler::onTick(double mspt_ms)
{
    impl_->tick(mspt_ms);
}

bool AllocationSampler::snapshot(AllocationSnapshot &snapshot, std::string &error)
{
    return impl_->captureSnapshot(snapshot, error);
}

AllocationDiagnostics AllocationSampler::diagnostics() const
{
    AllocationDiagnostics result;
    result.supported = true;
    result.accounting_state = impl_->accounting_state.load(std::memory_order_acquire);
    result.live_index_capacity = liveIndexCapacity();
    result.live_record_capacity = liveRecordCapacity();
    result.drain_truncated = impl_->drain_truncated.load(std::memory_order_relaxed);
    result.drain_truncated_allocation_events = impl_->drain_truncated_allocation_events.load(std::memory_order_relaxed);
    result.drain_truncated_thread_observation_events =
        impl_->drain_truncated_thread_observation_events.load(std::memory_order_relaxed);
    result.drain_truncated_tick_events = impl_->drain_truncated_tick_events.load(std::memory_order_relaxed);
    result.retained_allocations_skipped = impl_->retained_allocations_skipped.load(std::memory_order_relaxed);
    result.record_pool_acquisition_failures = impl_->record_pool_acquisition_failures.load(std::memory_order_relaxed);
    result.insertion_contention_failures = impl_->insertion_contention_failures.load(std::memory_order_relaxed);
    result.exhausted_insertion_probe_failures =
        impl_->exhausted_insertion_probe_failures.load(std::memory_order_relaxed);
    result.detach_contention_attempts = impl_->detach_contention_attempts.load(std::memory_order_relaxed);
    result.processed_allocation_events = impl_->processed_allocation_events.load(std::memory_order_relaxed);
    result.processed_thread_observation_events =
        impl_->processed_thread_observation_events.load(std::memory_order_relaxed);
    result.processed_tick_events = impl_->processed_tick_events.load(std::memory_order_relaxed);
    result.discarded_allocation_events = impl_->discarded_allocation_events.load(std::memory_order_relaxed);
    result.discarded_thread_observation_events =
        impl_->discarded_thread_observation_events.load(std::memory_order_relaxed);
    result.discarded_tick_events = impl_->discarded_tick_events.load(std::memory_order_relaxed);
    result.consumer_lifetime_elapsed_ns = impl_->consumer_lifetime_elapsed_ns.load(std::memory_order_relaxed);
    result.active_drain_elapsed_ns = impl_->active_drain_elapsed_ns.load(std::memory_order_relaxed);
    result.caller_final_drain_elapsed_ns = impl_->caller_final_drain_elapsed_ns.load(std::memory_order_relaxed);
    result.caller_final_drain_allocation_events =
        impl_->caller_final_drain_allocation_events.load(std::memory_order_relaxed);
    result.caller_final_drain_thread_observation_events =
        impl_->caller_final_drain_thread_observation_events.load(std::memory_order_relaxed);
    result.caller_final_drain_tick_events = impl_->caller_final_drain_tick_events.load(std::memory_order_relaxed);
    result.module_cache_capacity = moduleCacheCapacity();
    return result;
}

AllocationDiagnosticsSnapshot AllocationSampler::allocationDiagnostics() const
{
    return diagnostics();
}

bool AllocationSampler::setCurrentThreadTrackingSuppressed(bool suppressed) noexcept
{
    return impl_->setCurrentThreadTrackingSuppressed(suppressed);
}
const CallTree &AllocationSampler::tree() const
{
    return impl_->aggregation.tree();
}
const std::map<std::uint64_t, ThreadCallTree> &AllocationSampler::threadTrees() const
{
    return impl_->aggregation.threadTrees();
}
const ModuleTable &AllocationSampler::modules() const
{
    return impl_->aggregation.modules();
}
const std::map<std::int32_t, WindowTickStats> &AllocationSampler::windowTicks() const
{
    return impl_->aggregation.windowTicks();
}
std::uint64_t AllocationSampler::numberOfTicks() const
{
    return impl_->current_tick.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::hookCalls() const
{
    std::uint64_t total = 0;
    for (const auto &counters : impl_->hot_counters) {
        total += counters.hook_calls.load(std::memory_order_relaxed);
    }
    return total;
}
std::uint64_t AllocationSampler::successfulAllocationCalls() const
{
    std::uint64_t total = 0;
    for (const auto &counters : impl_->hot_counters) {
        total += counters.successful_allocation_calls.load(std::memory_order_relaxed);
    }
    return total;
}
std::uint64_t AllocationSampler::sampleCount() const
{
    return impl_->aggregation.sampleCount();
}
std::uint64_t AllocationSampler::samplingPoints() const
{
    return impl_->sampling_points.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::sampledBytes() const
{
    return impl_->aggregation.sampledBytes();
}
std::uint64_t AllocationSampler::filteredSamples() const
{
    return impl_->filtered_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::threadNameFailures() const
{
    return impl_->aggregation.threadNameFailures();
}
std::uint64_t AllocationSampler::threadIdentityCacheDrops() const
{
    return impl_->aggregation.threadIdentityCacheDrops();
}
std::uint64_t AllocationSampler::observedBytes() const
{
    std::uint64_t total = 0;
    for (const auto &counters : impl_->hot_counters) {
        total += counters.observed_bytes.load(std::memory_order_relaxed);
    }
    return total;
}
std::uint64_t AllocationSampler::droppedSamples() const
{
    return impl_->dropped_samples.load(std::memory_order_relaxed) + impl_->aggregation.droppedSamples();
}
std::uint64_t AllocationSampler::droppedEvents() const
{
    return impl_->dropped_events.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::droppedTickEvents() const
{
    return impl_->dropped_tick_events.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::tickEventCapacity()
{
    return KTickEventCapacity;
}
std::uint64_t AllocationSampler::enqueuedSamples() const
{
    return impl_->enqueued_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::eventQueueHighWaterMark() const
{
    return impl_->events.high_water.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::eventQueueCapacity()
{
    return KEventCapacity;
}
std::uint64_t AllocationSampler::freedSamples() const
{
    return impl_->freed_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::freedBytes() const
{
    return impl_->freed_bytes.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::liveSamples() const
{
    return impl_->live_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::liveBytes() const
{
    return impl_->live_bytes.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::peakLiveSamples() const
{
    return impl_->peak_live_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::liveIndexCapacity()
{
    return KLiveIndexCapacity;
}
std::uint64_t AllocationSampler::liveRecordCapacity()
{
    return KEventCapacity;
}
std::uint64_t AllocationSampler::moduleCacheCapacity()
{
    return 0;
}
std::uint64_t AllocationSampler::sampledThreadCount() const
{
    return impl_->aggregation.threadTrees().size();
}
std::uint64_t AllocationSampler::threadRootCapacity()
{
    return AllocationProfileAggregation::kThreadRootCapacity;
}
std::uint64_t AllocationSampler::overflowThreadCount() const
{
    return impl_->overflow_threads.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::threadStateDrops() const
{
    return impl_->thread_state_drops.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::hookedModuleCount() const
{
    return impl_->hooked_module_count.load(std::memory_order_acquire);
}
std::uint64_t AllocationSampler::skippedModuleCount() const
{
    return impl_->skipped_module_count.load(std::memory_order_acquire);
}
std::uint64_t AllocationSampler::failedModuleCount() const
{
    return impl_->failed_module_count.load(std::memory_order_acquire);
}
std::uint64_t AllocationSampler::moduleRegistryCount() const
{
    return impl_->aggregation.modules().size();
}
std::uint64_t AllocationSampler::moduleRegistryCapacity()
{
    return AllocationProfileAggregation::kModuleCapacity;
}
std::uint64_t AllocationSampler::profileNodeCapacity()
{
    return AllocationProfileAggregation::kProfileNodeCapacity;
}

std::uint64_t AllocationSampler::profileTimeEntryCapacity()
{
    return AllocationProfileAggregation::kProfileTimeEntryCapacity;
}

std::uint64_t AllocationSampler::profileStorageSampleDrops() const
{
    return impl_->aggregation.droppedProfileSamples();
}

bool AllocationSampler::profileStorageExhausted() const
{
    return impl_->aggregation.profileStorageExhausted();
}

std::uint64_t AllocationSampler::pendingSampleCapacity()
{
    return AllocationProfileAggregation::kPendingSampleCapacity;
}

std::uint64_t AllocationSampler::pendingSampleDrops() const
{
    return impl_->aggregation.pendingSampleDrops();
}

std::uint64_t AllocationSampler::pendingCapacityDrops() const
{
    return impl_->aggregation.pendingCapacityDrops();
}

std::uint64_t AllocationSampler::pendingStaleDrops() const
{
    return impl_->aggregation.pendingStaleDrops();
}

std::uint64_t AllocationSampler::pendingFinalDrops() const
{
    return impl_->aggregation.pendingFinalDrops();
}

std::uint64_t AllocationSampler::terminalInFlightTickSamplesDiscarded() const
{
    return impl_->aggregation.terminalInFlightTickSamplesDiscarded();
}

std::uint64_t AllocationSampler::moduleOverflowFrames() const
{
    return impl_->aggregation.moduleOverflowFrames();
}

std::uint64_t AllocationSampler::retainedHistoryWindows() const
{
    return impl_->aggregation.retainedHistoryWindows();
}

std::uint64_t AllocationSampler::historySamplesPruned() const
{
    return impl_->aggregation.historySamplesPruned();
}

std::uint64_t AllocationSampler::historyBytesPruned() const
{
    return impl_->aggregation.historyBytesPruned();
}

bool AllocationSampler::historyTruncated() const
{
    return impl_->aggregation.historyTruncated();
}

bool AllocationSampler::dataIncomplete() const
{
    return impl_->dropped_samples.load(std::memory_order_relaxed) != 0 ||
           impl_->lifecycle_dropped.load(std::memory_order_relaxed) != 0 ||
           impl_->contention_dropped.load(std::memory_order_relaxed) != 0 ||
           impl_->dropped_tick_events.load(std::memory_order_relaxed) != 0 ||
           impl_->thread_state_drops.load(std::memory_order_relaxed) != 0 ||
           impl_->aggregation.threadIdentityCacheDrops() != 0 || impl_->aggregation.dataIncomplete() ||
           impl_->failed_module_count.load(std::memory_order_acquire) != 0;
}
std::uint64_t AllocationSampler::averageLifetimeMs() const
{
    const std::uint64_t count = impl_->freed_samples.load(std::memory_order_relaxed);
    return count == 0 ? 0 : impl_->lifetime_ms_total.load(std::memory_order_relaxed) / count;
}
std::uint64_t AllocationSampler::maximumLifetimeMs() const
{
    return impl_->lifetime_ms_max.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::lifecycleDropped() const
{
    return impl_->lifecycle_dropped.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::contentionDropped() const
{
    return impl_->contention_dropped.load(std::memory_order_relaxed);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::uint64_t AllocationSampler::drainTruncated() const
{
    return impl_->drain_truncated.load(std::memory_order_relaxed);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool AllocationSampler::stopWaitTimedOut() const
{
    return impl_->stop_wait_timed_out.load(std::memory_order_acquire);
}

bool AllocationSampler::aggregatorMayBeAlive() const
{
    return impl_->aggregator_thread.joinable();
}

bool AllocationSampler::backendCleanupPending() const
{
    return impl_->backendCleanupPending();
}
std::uint64_t AllocationSampler::retainedAverageAgeMs() const
{
    const std::uint64_t count = impl_->aggregation.sampleCount();
    return count == 0 ? 0 : impl_->retained_age_ms_total.load(std::memory_order_relaxed) / count;
}
std::uint64_t AllocationSampler::retainedMaximumAgeMs() const
{
    return impl_->retained_age_ms_max.load(std::memory_order_relaxed);
}
bool AllocationSampler::running() const
{
    return impl_->running.load(std::memory_order_acquire) && !impl_->stop_requested.load(std::memory_order_acquire);
}
bool AllocationSampler::hooksInstalled() const
{
    return impl_->hooks_installed.load(std::memory_order_acquire);
}
bool AllocationSampler::failure(std::string &error) const
{
    if (!impl_->aggregator_failed.load(std::memory_order_acquire)) {
        error.clear();
        return false;
    }
    error = impl_->aggregatorFailureMessage();
    return true;
}
const char *AllocationSampler::backendId() noexcept
{
    return "native-glibc/elf-import";
}

const char *AllocationSampler::backendName() noexcept
{
    return "Linux glibc/ELF import slots";
}

const std::vector<AllocationHookCapability> &AllocationSampler::hookCapabilities() const
{
    return impl_->hook_capabilities;
}
std::size_t AllocationSampler::hookTargetCount() const
{
    return impl_->hook_target_count.load(std::memory_order_acquire);
}

}  // namespace spark
