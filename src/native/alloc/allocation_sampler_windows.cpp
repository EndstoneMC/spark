#include "native/alloc/allocation_sampler.h"

#ifndef _WIN32
#error "allocation_sampler_windows.cpp must only be compiled on Windows"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: Windows SDK headers require windows.h first
#include <windows.h>
#include <psapi.h>
#include <winternl.h>
// clang-format on

#include "native/alloc/allocation_profile_aggregation.h"
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
#include "native/alloc/allocation_diagnostics_test_access.h"
#include "native/alloc/allocation_lifecycle_test_access.h"
#endif
#include "native/alloc/allocation_quiescence.h"
#include "native/alloc/bounded_event_queue.h"
#include "native/alloc/byte_sampler.h"
#include "native/alloc/stable_shard_snapshot.h"
#include "native/alloc/windows_allocation_iat_hooks.h"
#include "native/alloc/windows_dynamic_stack_capture.h"
#include "native/sampler/thread_info.h"
#include "profiling_window.h"

#ifdef SPARK_ALLOCATION_LAST_ERROR_TESTING
#define SPARK_ALLOCATION_LAST_ERROR_TEST_IMPLEMENTATION
#include "native/alloc/windows_allocation_last_error_test.inc"
#undef SPARK_ALLOCATION_LAST_ERROR_TEST_IMPLEMENTATION
#endif

namespace spark {
namespace {

constexpr std::size_t KStackDepth = 48;
constexpr std::size_t KEventCapacity = 16384;
constexpr std::size_t KLiveIndexCapacity = KEventCapacity * 2;
constexpr std::size_t KLiveIndexShards = 64;
constexpr std::size_t KLiveIndexShardCapacity = KLiveIndexCapacity / KLiveIndexShards;
constexpr std::size_t KWindowsHotCounterShards = 256;
static_assert((KWindowsHotCounterShards & (KWindowsHotCounterShards - 1)) == 0);
constexpr std::size_t KMaxSampledThreads = 256;
constexpr std::size_t KMaxThreadStates = 2048;

#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
struct AllocationThreadCreationFailureForTesting {};

void yieldLifecycleTest() noexcept
{
    ::SwitchToThread();
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
        std::scoped_lock lock(gate.seed_mutex);
    }
    catch (...) {
        gate.timed_out.store(true, std::memory_order_release);
        return false;
    }
    return !gate.timed_out.load(std::memory_order_acquire);
}
#endif

constexpr std::size_t KMaxAllocationModules = 512;
constexpr std::size_t KMaxModuleCacheEntries = 1024;

struct MainImageRange {
    std::uintptr_t base = 0;
    std::uintptr_t size = 0;

    static MainImageRange validated(std::uintptr_t base, std::uintptr_t size) noexcept
    {
        if (base == 0 || size == 0 || size > std::numeric_limits<std::uintptr_t>::max() - base) {
            return {};
        }
        return {.base = base, .size = size};
    }

    [[nodiscard]] bool contains(std::uintptr_t address) const noexcept
    {
        return address >= base && address - base < size;
    }
};
constexpr std::size_t KMaxProfileNodes = 131072;
constexpr std::size_t KMaxPendingSamples = 32768;
constexpr std::size_t KMaxTickDecisions = 100000;
constexpr std::size_t KTickEventCapacity = 4096;
constexpr std::uint32_t KFramesToSkip = 2;
constexpr std::uint64_t KHookRefreshIntervalMs = 2000;
constexpr std::uint64_t KDrainBudgetMs = 1500;
constexpr std::size_t KDrainBudgetEvents = KEventCapacity;
constexpr std::uint64_t KDrainOvershootAllowanceMs = 500;
constexpr std::uint64_t KAggregatorExitTimeoutMs = 2000;
static_assert(KAggregatorExitTimeoutMs >= KDrainBudgetMs + KDrainOvershootAllowanceMs,
              "the aggregator exit deadline must cover a full drain budget plus one in-flight event");

void *tombstonePointer() noexcept
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(1));
}

struct RegisteredHookTarget {
    void *address = nullptr;
    std::string export_name;
};

std::uint64_t monotonicMs() noexcept
{
    return static_cast<std::uint64_t>(::GetTickCount64());
}

std::uint64_t monotonicNs() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

bool fileTimeToNanoseconds(std::uint32_t high, std::uint32_t low, std::uint64_t &value) noexcept
{
    const std::uint64_t units = (static_cast<std::uint64_t>(high) << 32) | static_cast<std::uint64_t>(low);
    constexpr std::uint64_t k_nanoseconds_per_file_time_unit = 100;
    if (units > std::numeric_limits<std::uint64_t>::max() / k_nanoseconds_per_file_time_unit) {
        value = 0;
        return false;
    }
    value = units * k_nanoseconds_per_file_time_unit;
    return true;
}

std::uint64_t saturatingMultiply(std::uint64_t a, std::uint64_t b) noexcept
{
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    if (a == 0 || b == 0) {
        return 0;
    }
    return a > max / b ? max : a * b;
}

bool checkedMultiply(std::size_t a, std::size_t b, std::uint64_t &out) noexcept
{
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        out = 0;
        return false;
    }
    out = static_cast<std::uint64_t>(a * b);
    return true;
}

std::string moduleBasename(const std::string &path)
{
    const std::size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool equalsIgnoreCase(const std::string &a, const char *b)
{
    return ::_stricmp(a.c_str(), b) == 0;
}

bool startsWithIgnoreCase(const std::string &value, const char *prefix)
{
    const std::size_t prefix_length = std::strlen(prefix);
    return value.size() >= prefix_length && ::_strnicmp(value.c_str(), prefix, prefix_length) == 0;
}

bool isSparkAllocationInstrumentation(const std::string &path)
{
    const std::string name = moduleBasename(path);
    return equalsIgnoreCase(name, "spark.dll") || equalsIgnoreCase(name, "endstone_spark.dll") ||
           startsWithIgnoreCase(name, "endstone_spark-");
}

bool isLeadingAllocatorRuntime(const std::string &path)
{
    const std::string name = moduleBasename(path);
    return equalsIgnoreCase(name, "ucrtbase.dll") || equalsIgnoreCase(name, "vcruntime140.dll") ||
           equalsIgnoreCase(name, "vcruntime140_1.dll") || equalsIgnoreCase(name, "msvcp140.dll");
}

}  // namespace

struct AllocationSampler::Impl {
    using MallocFn = void *(__cdecl *)(std::size_t);
    using CallocFn = void *(__cdecl *)(std::size_t, std::size_t);
    using ReallocFn = void *(__cdecl *)(void *, std::size_t);
    using RecallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t);
    using FreeFn = void(__cdecl *)(void *);
    using AlignedMallocFn = void *(__cdecl *)(std::size_t, std::size_t);
    using AlignedReallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t);
    using AlignedRecallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t, std::size_t);
    using AlignedOffsetMallocFn = void *(__cdecl *)(std::size_t, std::size_t, std::size_t);
    using AlignedOffsetReallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t, std::size_t);
    using AlignedOffsetRecallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t, std::size_t, std::size_t);
    using HeapAllocFn = void *(WINAPI *)(HANDLE, DWORD, SIZE_T);
    using HeapReAllocFn = void *(WINAPI *)(HANDLE, DWORD, void *, SIZE_T);
    using HeapFreeFn = BOOL(WINAPI *)(HANDLE, DWORD, void *);

    struct alignas(MEMORY_ALLOCATION_ALIGNMENT) AllocationEvent {
        SLIST_ENTRY entry{};
        std::uint64_t weight_bytes = 0;
        std::uint64_t tick_id = 0;
        std::uint64_t thread_id = 0;
        std::uint64_t os_thread_id = 0;
        std::int32_t window = 0;
        std::uint16_t depth = 0;
        bool thread_observation = false;
        void *frames[KStackDepth]{};
    };

    struct TickEvent {
        std::uint64_t tick_id = 0;
        double mspt_ms = 0.0;
    };

    struct alignas(MEMORY_ALLOCATION_ALIGNMENT) LiveAllocation {
        SLIST_ENTRY entry{};
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
        void *frames[KStackDepth]{};
    };

    struct LiveIndexEntry {
        void *pointer = nullptr;
        std::uint64_t allocation_id = 0;
        LiveAllocation *allocation = nullptr;
    };

    static_assert(alignof(AllocationEvent) >= MEMORY_ALLOCATION_ALIGNMENT);
    static_assert(sizeof(AllocationEvent) % MEMORY_ALLOCATION_ALIGNMENT == 0);
    static_assert(alignof(LiveAllocation) >= MEMORY_ALLOCATION_ALIGNMENT);
    static_assert(sizeof(LiveAllocation) % MEMORY_ALLOCATION_ALIGNMENT == 0);

    struct alignas(64) HookCounter {
        std::atomic<std::uint64_t> value{0};
    };

    struct alignas(64) HotCounters {
        std::atomic<std::uint64_t> hook_calls{0};
        std::atomic<std::uint64_t> successful_allocation_calls{0};
        std::atomic<std::uint64_t> observed_bytes{0};
        std::atomic<std::uint64_t> tracking_hook_calls{0};
    };
    static_assert(sizeof(HotCounters) == 64);

    static std::atomic<Impl *> mActiveInstance;
    static std::array<HookCounter, 64> mActiveHookCalls;

    struct ThreadSamplingState {
        // 0 free, 1 initializing, 2 active, 3 being checked for reclamation.
        std::atomic<std::uint8_t> registry_state{0};
        std::atomic<void *> teb{nullptr};
        HANDLE thread_handle = nullptr;
        ByteSamplingState bytes;
        std::uint64_t identity_generation = 0;
        std::uint64_t session_thread_id = 0;
        std::uint64_t os_thread_id = 0;
        bool inside_hook = false;
        bool tracking_suppressed = false;
        bool identity_announced = false;
    };

    struct HeapCallCleanupState {
        HookCounter *hook_counter = nullptr;
        HotCounters *tracking_counters = nullptr;
        ThreadSamplingState *recursion_state = nullptr;
        LiveAllocation *pending_detached = nullptr;
        DWORD entry_last_error = 0;
        DWORD original_last_error = 0;
        void *result = nullptr;
        bool hook_owned = false;
        bool tracking_owned = false;
        bool recursion_owned = false;
        bool count_only_recursion_owned = false;
        bool original_completed = false;
    };
    static_assert(std::is_trivially_destructible_v<HeapCallCleanupState>);

    inline static thread_local bool mCountOnlyInsideHook = false;

    static std::size_t currentHotCounterShard() noexcept
    {
        std::uintptr_t value = reinterpret_cast<std::uintptr_t>(::NtCurrentTeb()) >> 12;
        value ^= value >> 17;
        value *= 0x9e3779b97f4a7c15ULL;
        value ^= value >> 29;
        return static_cast<std::size_t>(value & (KWindowsHotCounterShards - 1));
    }

    HotCounters &hotCountersForCurrentThread() noexcept { return hot_counters[currentHotCounterShard()]; }

    class HookCallGuard {
    public:
        HookCallGuard() noexcept
        {
            const std::uintptr_t thread_key = reinterpret_cast<std::uintptr_t>(::NtCurrentTeb()) >> 12;
            counter_ = &mActiveHookCalls[static_cast<std::size_t>(thread_key) % mActiveHookCalls.size()];
            counter_->value.fetch_add(1, std::memory_order_acq_rel);
        }

        ~HookCallGuard() { counter_->value.fetch_sub(1, std::memory_order_release); }

        HookCallGuard(const HookCallGuard &) = delete;
        HookCallGuard &operator=(const HookCallGuard &) = delete;

    private:
        HookCounter *counter_ = nullptr;
    };

    class TrackingCallGuard {
    public:
        explicit TrackingCallGuard(Impl &impl) noexcept : impl_(impl), counters_(&impl.hotCountersForCurrentThread())
        {
            if (!impl_.tracking.load(std::memory_order_acquire)) {
                return;
            }
            counters_->tracking_hook_calls.fetch_add(1, std::memory_order_acq_rel);
            if (impl_.tracking.load(std::memory_order_acquire)) {
                active_ = true;
                return;
            }
            counters_->tracking_hook_calls.fetch_sub(1, std::memory_order_release);
        }

        ~TrackingCallGuard()
        {
#ifdef SPARK_ALLOCATION_LAST_ERROR_TESTING
            ::SetLastError(0xBADC0DE);
#endif
            if (active_) {
                counters_->tracking_hook_calls.fetch_sub(1, std::memory_order_release);
            }
        }

        explicit operator bool() const noexcept { return active_; }

    private:
        Impl &impl_;
        HotCounters *counters_ = nullptr;
        bool active_ = false;
    };

    class AllocatorLastErrorGuard {
    public:
        AllocatorLastErrorGuard() noexcept : error_(::GetLastError()) {}
        ~AllocatorLastErrorGuard() { ::SetLastError(error_); }

        template <typename Function, typename... Args>
        auto call(Function function, Args... args) -> decltype(function(args...))
        {
            ::SetLastError(error_);
            struct LastErrorCapture {
                DWORD &error;
                ~LastErrorCapture() noexcept { error = ::GetLastError(); }
            } capture{error_};
            return function(args...);
        }

        void call(FreeFn function, void *pointer) noexcept
        {
            ::SetLastError(error_);
            function(pointer);
            error_ = ::GetLastError();
        }

        AllocatorLastErrorGuard(const AllocatorLastErrorGuard &) = delete;
        AllocatorLastErrorGuard &operator=(const AllocatorLastErrorGuard &) = delete;

    private:
        DWORD error_;
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
#ifdef SPARK_ALLOCATION_LAST_ERROR_TESTING
            ::SetLastError(0xBADC0DE);
#endif
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

    class TrackingSuppressionGuard {
    public:
        explicit TrackingSuppressionGuard(Impl &impl) noexcept : state_(impl.currentThreadState())
        {
            if (state_ != nullptr) {
                previous_ = state_->tracking_suppressed;
                state_->tracking_suppressed = true;
            }
        }

        ~TrackingSuppressionGuard()
        {
            if (state_ != nullptr) {
                state_->tracking_suppressed = previous_;
            }
        }

    private:
        ThreadSamplingState *state_ = nullptr;
        bool previous_ = false;
    };

    class DetachedAllocationRollback {
    public:
        DetachedAllocationRollback(Impl &impl, LiveAllocation *allocation) noexcept
            : impl_(impl), allocation_(allocation)
        {
        }

        ~DetachedAllocationRollback() noexcept
        {
            if (armed_) {
                impl_.restoreDetachedAllocation(allocation_);
            }
        }

        DetachedAllocationRollback(const DetachedAllocationRollback &) = delete;
        DetachedAllocationRollback &operator=(const DetachedAllocationRollback &) = delete;

        void disarm() noexcept { armed_ = false; }

    private:
        Impl &impl_;
        LiveAllocation *allocation_ = nullptr;
        bool armed_ = true;
    };

    std::unique_ptr<WindowsAllocationIatHooks> hooks;
    bool hooks_configured = false;
    DWORD tls_index = TLS_OUT_OF_INDEXES;
    std::atomic<bool> hooks_installed{false};
    std::atomic<bool> tracking{false};
    std::atomic<bool> running{false};
    std::atomic<AllocationAccountingState> accounting_state{AllocationAccountingState::NotStarted};
    std::atomic<bool> tick_admission_open{true};
    std::atomic<bool> finalize_pending{false};
    std::atomic<bool> pending_finalized{false};
    std::atomic<std::uint64_t> terminal_tick{0};
    std::atomic<bool> aggregator_running{false};
    std::atomic<bool> aggregator_failed{false};
    std::atomic<bool> drain_abort{false};
    std::atomic<bool> aggregator_exited{false};
    std::atomic<bool> stop_wait_timed_out{false};
    std::atomic<bool> backend_cleanup_pending{false};
    std::atomic<bool> backend_shutdown_pending{false};
    std::array<char, 256> aggregator_failure{};

    MallocFn real_malloc = nullptr;
    CallocFn real_calloc = nullptr;
    ReallocFn real_realloc = nullptr;
    RecallocFn real_recalloc = nullptr;
    FreeFn real_free = nullptr;
    AlignedMallocFn real_aligned_malloc = nullptr;
    AlignedReallocFn real_aligned_realloc = nullptr;
    AlignedRecallocFn real_aligned_recalloc = nullptr;
    AlignedOffsetMallocFn real_aligned_offset_malloc = nullptr;
    AlignedOffsetReallocFn real_aligned_offset_realloc = nullptr;
    AlignedOffsetRecallocFn real_aligned_offset_recalloc = nullptr;
    FreeFn real_aligned_free = nullptr;

    // UCRT internal base exports are optional. Hooking them catches direct callers;
    // nested calls from public wrappers are suppressed by RecursionGuard.
    MallocFn real_malloc_base = nullptr;
    CallocFn real_calloc_base = nullptr;
    ReallocFn real_realloc_base = nullptr;
    FreeFn real_free_base = nullptr;
    HeapAllocFn real_heap_alloc = nullptr;
    HeapReAllocFn real_heap_realloc = nullptr;
    HeapFreeFn real_heap_free = nullptr;

    std::mutex lifecycle_mutex;
    std::mutex tick_mutex;
    std::timed_mutex aggregate_mutex;
    std::vector<RegisteredHookTarget> registered_targets;
    std::vector<AllocationHookCapability> hook_capabilities;
    AllocationSamplerConfig config{};
    std::atomic<std::uint64_t> current_tick{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> interval_bytes{kDefaultAllocationIntervalBytes};
    std::atomic<std::uint64_t> sampling_seed{0};
    std::array<HotCounters, KWindowsHotCounterShards> hot_counters{};
    std::atomic<std::uint64_t> sampling_points{0};
    std::atomic<std::uint64_t> filtered_samples{0};
    std::atomic<std::uint64_t> dropped_samples{0};
    std::atomic<std::uint64_t> dropped_events{0};
    std::atomic<std::uint64_t> dropped_tick_events{0};
    std::atomic<std::uint64_t> enqueued_samples{0};
    std::atomic<std::uint64_t> ready_event_count{0};
    std::atomic<std::uint64_t> ready_event_high_water{0};
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
    std::atomic<std::uint64_t> drain_truncated{0};
    std::atomic<std::uint64_t> lifecycle_version{0};
    std::atomic<std::uint64_t> lifecycle_readers{0};
    std::atomic<std::uint64_t> lifecycle_writers{0};
    std::atomic<std::uint64_t> retained_age_ms_total{0};
    std::atomic<std::uint64_t> retained_age_ms_max{0};
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
    std::atomic<bool> aggregator_cpu_supported{true};
    std::atomic<bool> aggregator_cpu_valid{false};
    std::atomic<bool> aggregator_cpu_read_failure{false};
    std::atomic<std::uint64_t> aggregator_cpu_time_ns{0};
    std::atomic<std::uint64_t> module_cache_hits{0};
    std::atomic<std::uint64_t> module_cache_misses{0};
    std::atomic<std::uint64_t> module_cache_insertion_refusals{0};
    std::atomic<std::uint64_t> module_cache_size{0};
    SLIST_HEADER free_events{};
    SLIST_HEADER ready_events{};
    AllocationEvent *event_storage = nullptr;
    SLIST_HEADER free_live_allocations{};
    SLIST_HEADER deferred_live_allocations{};
    LiveAllocation *live_storage = nullptr;
    LiveIndexEntry *live_index = nullptr;
    std::array<SRWLOCK, KLiveIndexShards> live_index_locks{};
    std::array<ThreadSamplingState, KMaxThreadStates> thread_states{};

    std::thread aggregator_thread;
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
    test::StartFailureGate *start_failure_gate_for_testing = nullptr;
    test::AllocationEventProcessingGate *event_processing_gate_for_testing = nullptr;
    bool force_process_event_failure_for_testing = false;
    bool force_drain_deadline_for_testing = false;
    bool force_retained_walk_budget_for_testing = false;
    std::atomic<std::uint64_t> retained_walk_visits_for_testing{0};
    bool force_aggregator_cpu_read_failure_for_testing = false;
    bool force_aggregator_cpu_zero_for_testing = false;
    bool fixture_no_hooks_for_testing = false;
    bool fixture_no_worker_for_testing = false;
    bool fixture_controls_configured_for_testing = false;
    test::AllocationFixtureWorkerGate *fixture_worker_gate_for_testing = nullptr;
    test::AllocationFixtureCpuWorkControl *fixture_cpu_work_for_testing = nullptr;
    ThreadSamplingState fixture_thread_state_for_testing{};
    std::uint64_t fixture_thread_owner_for_testing = 0;
    bool fixture_thread_state_active_for_testing = false;
    bool force_main_image_discovery_failure_for_testing = false;
    std::uint64_t main_image_discoveries_for_testing = 0;
    std::uint64_t fallback_queries_for_testing = 0;
#endif
    BoundedEventQueue<TickEvent, KTickEventCapacity> ticks;

    AllocationProfileAggregation aggregation;
    std::unordered_map<std::uintptr_t, ModuleId> module_cache;
    MainImageRange main_image_range;

    std::atomic<RecoverySink *> recovery_sink{nullptr};

    ~Impl() = default;

    static Impl *active() noexcept { return mActiveInstance.load(std::memory_order_acquire); }

    static Impl *activeOrAbort() noexcept
    {
        Impl *self = active();
        if (self == nullptr) {
            std::abort();
        }
        if (self->tracking.load(std::memory_order_relaxed)) {
            self->hotCountersForCurrentThread().hook_calls.fetch_add(1, std::memory_order_relaxed);
        }
        return self;
    }

    static void acquireHeapHookCall(HeapCallCleanupState &cleanup) noexcept
    {
        const std::uintptr_t thread_key = reinterpret_cast<std::uintptr_t>(::NtCurrentTeb()) >> 12;
        cleanup.hook_counter = &mActiveHookCalls[static_cast<std::size_t>(thread_key) % mActiveHookCalls.size()];
        cleanup.hook_counter->value.fetch_add(1, std::memory_order_acq_rel);
        cleanup.hook_owned = true;
    }

    static void releaseHeapHookCall(HeapCallCleanupState &cleanup) noexcept
    {
        if (cleanup.hook_owned) {
            cleanup.hook_counter->value.fetch_sub(1, std::memory_order_release);
            cleanup.hook_owned = false;
        }
    }

    void acquireHeapTracking(HeapCallCleanupState &cleanup) noexcept
    {
        if (!tracking.load(std::memory_order_acquire)) {
            return;
        }
        cleanup.tracking_counters = &hotCountersForCurrentThread();
        cleanup.tracking_counters->tracking_hook_calls.fetch_add(1, std::memory_order_acq_rel);
        if (tracking.load(std::memory_order_acquire)) {
            cleanup.tracking_owned = true;
        }
        else {
            cleanup.tracking_counters->tracking_hook_calls.fetch_sub(1, std::memory_order_release);
            cleanup.tracking_counters = nullptr;
        }
    }

    void acquireHeapRecursion(HeapCallCleanupState &cleanup) noexcept
    {
        if (config.count_only) {
            if (!mCountOnlyInsideHook) {
                mCountOnlyInsideHook = true;
                cleanup.count_only_recursion_owned = true;
                cleanup.recursion_owned = true;
            }
            return;
        }

        ThreadSamplingState *state = currentThreadState();
        if (state != nullptr && !state->inside_hook) {
            state->inside_hook = true;
            cleanup.recursion_state = state;
            cleanup.recursion_owned = true;
        }
    }

    static void releaseHeapRecursion(HeapCallCleanupState &cleanup) noexcept
    {
        if (!cleanup.recursion_owned) {
            return;
        }
        if (cleanup.count_only_recursion_owned) {
            mCountOnlyInsideHook = false;
        }
        else if (cleanup.recursion_state != nullptr) {
            cleanup.recursion_state->inside_hook = false;
        }
        cleanup.recursion_owned = false;
    }

    static void releaseHeapTracking(HeapCallCleanupState &cleanup) noexcept
    {
        if (cleanup.tracking_owned) {
            cleanup.tracking_counters->tracking_hook_calls.fetch_sub(1, std::memory_order_release);
            cleanup.tracking_owned = false;
        }
    }

    static void *executeHeapAlloc(Impl *initial, HeapAllocFn function, HANDLE heap, DWORD flags, SIZE_T requested_size,
                                  bool from_hook)
    {
        HeapCallCleanupState cleanup{};
        cleanup.entry_last_error = ::GetLastError();
        cleanup.original_last_error = cleanup.entry_last_error;
        Impl *self = initial;
        __try {
            if (from_hook) {
                acquireHeapHookCall(cleanup);
                self = activeOrAbort();
                function = self->real_heap_alloc;
            }

            if (self->shouldTrackCurrentThread()) {
                self->acquireHeapTracking(cleanup);
                if (cleanup.tracking_owned) {
                    self->acquireHeapRecursion(cleanup);
                }
            }

            __try {
                ::SetLastError(cleanup.entry_last_error);
                cleanup.result = function(heap, flags, requested_size);
            }
            __finally {
                cleanup.original_last_error = ::GetLastError();
            }

            if (cleanup.tracking_owned && cleanup.recursion_owned && cleanup.result != nullptr) {
                self->recordAllocation(cleanup.result, static_cast<std::uint64_t>(requested_size));
            }
        }
        __finally {
            releaseHeapRecursion(cleanup);
            releaseHeapTracking(cleanup);
            releaseHeapHookCall(cleanup);
            ::SetLastError(cleanup.original_last_error);
        }
        return cleanup.result;
    }

    static void *executeHeapReAlloc(Impl *initial, HeapReAllocFn function, HANDLE heap, DWORD flags, void *pointer,
                                    SIZE_T requested_size, bool from_hook)
    {
        HeapCallCleanupState cleanup{};
        cleanup.entry_last_error = ::GetLastError();
        cleanup.original_last_error = cleanup.entry_last_error;
        Impl *self = initial;
        __try {
            if (from_hook) {
                acquireHeapHookCall(cleanup);
                self = activeOrAbort();
                function = self->real_heap_realloc;
            }

            if (self->shouldTrackCurrentThread()) {
                self->acquireHeapTracking(cleanup);
                if (cleanup.tracking_owned) {
                    self->acquireHeapRecursion(cleanup);
                    if (cleanup.recursion_owned) {
                        cleanup.pending_detached = self->detachAllocation(pointer);
                    }
                }
            }

            __try {
                ::SetLastError(cleanup.entry_last_error);
                cleanup.result = function(heap, flags, pointer, requested_size);
            }
            __finally {
                cleanup.original_last_error = ::GetLastError();
            }
            cleanup.original_completed = true;

            if (cleanup.pending_detached != nullptr) {
                if (cleanup.result != nullptr) {
                    self->retireAllocation(cleanup.pending_detached, monotonicMs());
                }
                else {
                    self->restoreDetachedAllocation(cleanup.pending_detached);
                }
                cleanup.pending_detached = nullptr;
            }
            if (cleanup.tracking_owned && cleanup.recursion_owned && cleanup.result != nullptr && requested_size != 0) {
                self->recordAllocation(cleanup.result, static_cast<std::uint64_t>(requested_size));
            }
        }
        __finally {
            if (!cleanup.original_completed && cleanup.pending_detached != nullptr) {
                self->restoreDetachedAllocation(cleanup.pending_detached);
                cleanup.pending_detached = nullptr;
            }
            releaseHeapRecursion(cleanup);
            releaseHeapTracking(cleanup);
            releaseHeapHookCall(cleanup);
            ::SetLastError(cleanup.original_last_error);
        }
        return cleanup.result;
    }

    static void *__cdecl hookMalloc(std::size_t size)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleMalloc(self->real_malloc, size);
    }

    static void *__cdecl hookCalloc(std::size_t count, std::size_t size)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleCalloc(self->real_calloc, count, size);
    }

    static void *__cdecl hookRealloc(void *pointer, std::size_t size)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleRealloc(self->real_realloc, pointer, size);
    }

    static void *__cdecl hookRecalloc(void *pointer, std::size_t count, std::size_t size)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleRecalloc(self->real_recalloc, pointer, count, size);
    }

    static void __cdecl hookFree(void *pointer) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        self->handleFree(self->real_free, pointer);
    }

    static void *__cdecl hookAlignedMalloc(std::size_t size, std::size_t alignment)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedMalloc(self->real_aligned_malloc, size, alignment);
    }

    static void *__cdecl hookAlignedRealloc(void *pointer, std::size_t size, std::size_t alignment)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedRealloc(self->real_aligned_realloc, pointer, size, alignment);
    }

    static void *__cdecl hookAlignedRecalloc(void *pointer, std::size_t count, std::size_t size, std::size_t alignment)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedRecalloc(self->real_aligned_recalloc, pointer, count, size, alignment);
    }

    static void *__cdecl hookAlignedOffsetMalloc(std::size_t size, std::size_t alignment, std::size_t offset)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedOffsetMalloc(self->real_aligned_offset_malloc, size, alignment, offset);
    }

    static void *__cdecl hookAlignedOffsetRealloc(void *pointer, std::size_t size, std::size_t alignment,
                                                  std::size_t offset)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedOffsetRealloc(self->real_aligned_offset_realloc, pointer, size, alignment, offset);
    }

    static void *__cdecl hookAlignedOffsetRecalloc(void *pointer, std::size_t count, std::size_t size,
                                                   std::size_t alignment, std::size_t offset)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedOffsetRecalloc(self->real_aligned_offset_recalloc, pointer, count, size, alignment,
                                                 offset);
    }

    static void __cdecl hookAlignedFree(void *pointer) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        self->handleFree(self->real_aligned_free, pointer);
    }

    static void *__cdecl hookMallocBase(std::size_t size)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleMalloc(self->real_malloc_base, size);
    }

    static void *__cdecl hookCallocBase(std::size_t count, std::size_t size)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleCalloc(self->real_calloc_base, count, size);
    }

    static void *__cdecl hookReallocBase(void *pointer, std::size_t size)
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleRealloc(self->real_realloc_base, pointer, size);
    }

    static void __cdecl hookFreeBase(void *pointer) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        self->handleFree(self->real_free_base, pointer);
    }

    static void *WINAPI hookHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
    {
        return executeHeapAlloc(nullptr, nullptr, heap, flags, size, true);
    }

    static void *WINAPI hookHeapReAlloc(HANDLE heap, DWORD flags, void *pointer, SIZE_T size)
    {
        return executeHeapReAlloc(nullptr, nullptr, heap, flags, pointer, size, true);
    }

    static BOOL WINAPI hookHeapFree(HANDLE heap, DWORD flags, void *pointer) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleHeapFree(self->real_heap_free, heap, flags, pointer);
    }

    ThreadSamplingState *currentThreadState() noexcept
    {
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (fixture_no_hooks_for_testing && fixture_no_worker_for_testing && fixture_thread_state_active_for_testing) {
            if (static_cast<std::uint64_t>(::GetCurrentThreadId()) != fixture_thread_owner_for_testing) {
                return nullptr;
            }
            return &fixture_thread_state_for_testing;
        }
#endif
        if (tls_index == TLS_OUT_OF_INDEXES) {
            return nullptr;
        }

        PTEB teb = ::NtCurrentTeb();
        // Reserved1[11] is the TEB ThreadLocalStoragePointer slot; null while TLS is being initialized.
        if (teb == nullptr || teb->Reserved1[11] == nullptr) {
            return nullptr;
        }

        void *value = ::TlsGetValue(tls_index);
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
        if (address >= begin && address < end && state->registry_state.load(std::memory_order_acquire) != 0 &&
            state->teb.load(std::memory_order_acquire) == teb) {
            return state;
        }

        for (std::size_t i = 0; i < state_limit; ++i) {
            ThreadSamplingState &candidate = thread_states[i];
            if (candidate.registry_state.load(std::memory_order_acquire) == 1 &&
                candidate.teb.load(std::memory_order_acquire) == teb) {
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
            expected = 2;
            if (candidate.registry_state.compare_exchange_strong(expected, 3, std::memory_order_acq_rel)) {
                if (candidate.thread_handle != nullptr &&
                    ::WaitForSingleObject(candidate.thread_handle, 0) == WAIT_OBJECT_0) {
                    ::CloseHandle(candidate.thread_handle);
                    candidate.thread_handle = nullptr;
                    candidate.registry_state.store(1, std::memory_order_release);
                    claimed = &candidate;
                    break;
                }
                candidate.registry_state.store(2, std::memory_order_release);
            }
        }
        if (claimed == nullptr) {
            thread_state_drops.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        claimed->teb.store(teb, std::memory_order_release);
        claimed->bytes = {};
        claimed->identity_generation = 0;
        claimed->session_thread_id = 0;
        claimed->os_thread_id = static_cast<std::uint64_t>(::GetCurrentThreadId());
        claimed->inside_hook = true;
        claimed->tracking_suppressed = false;
        claimed->identity_announced = false;
        HANDLE thread_handle = nullptr;
        if (::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &thread_handle,
                              SYNCHRONIZE, FALSE, 0) != FALSE) {
            claimed->thread_handle = thread_handle;
        }
        if (::TlsSetValue(tls_index, claimed) == FALSE) {
            if (claimed->thread_handle != nullptr) {
                ::CloseHandle(claimed->thread_handle);
                claimed->thread_handle = nullptr;
            }
            claimed->inside_hook = false;
            claimed->teb.store(nullptr, std::memory_order_release);
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
        if (!tracking.load(std::memory_order_relaxed)) {
            return false;
        }
        if (config.count_only) {
            return true;
        }
        auto *self = const_cast<Impl *>(this);
        ThreadSamplingState *state = self->currentThreadState();
        return state != nullptr && !state->tracking_suppressed;
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

    void recycleLiveRecord(LiveAllocation *allocation) noexcept
    {
        SLIST_HEADER *destination = lifecycle_readers.load(std::memory_order_acquire) == 0 ? &free_live_allocations
                                                                                           : &deferred_live_allocations;
        ::InterlockedPushEntrySList(destination, &allocation->entry);
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
        std::uint64_t previous_max = lifetime_ms_max.load(std::memory_order_relaxed);
        while (previous_max < lifetime &&
               !lifetime_ms_max.compare_exchange_weak(previous_max, lifetime, std::memory_order_relaxed)) {
        }
        recycleLiveRecord(allocation);
    }

    LiveAllocation *detachAllocation(void *pointer) noexcept
    {
        if (pointer == nullptr || live_index == nullptr) {
            return nullptr;
        }
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t shard = liveIndexShard(hash);
        if (config.force_live_lock_contention_for_testing || !::TryAcquireSRWLockExclusive(&live_index_locks[shard])) {
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
                break;
            }
        }
        lifecycle_version.fetch_add(1, std::memory_order_release);
        lifecycle_writers.fetch_sub(1, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&live_index_locks[shard]);
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

    void handleFree(FreeFn function, void *pointer) noexcept
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            last_error.call(function, pointer);
            return;
        }
        TrackingCallGuard tracking_call(*this);
        RecursionGuard recursion(*this);
        if (!tracking_call || !recursion.owner()) {
            last_error.call(function, pointer);
            return;
        }
        LiveAllocation *allocation = detachAllocation(pointer);
        last_error.call(function, pointer);
        if (allocation != nullptr) {
            retireAllocation(allocation, monotonicMs());
        }
    }

    BOOL handleHeapFree(HeapFreeFn function, HANDLE heap, DWORD flags, void *pointer) noexcept
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            return last_error.call(function, heap, flags, pointer);
        }
        TrackingCallGuard tracking_call(*this);
        RecursionGuard recursion(*this);
        if (!tracking_call || !recursion.owner()) {
            return last_error.call(function, heap, flags, pointer);
        }
        LiveAllocation *allocation = detachAllocation(pointer);
        const BOOL result = last_error.call(function, heap, flags, pointer);
        if (result != FALSE) {
            if (allocation != nullptr) {
                retireAllocation(allocation, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(allocation);
        }
        return result;
    }

    void *handleMalloc(MallocFn function, std::size_t requested_size)
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            return last_error.call(function, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return last_error.call(function, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return last_error.call(function, requested_size);
        }
        void *pointer = last_error.call(function, requested_size);
        if (pointer != nullptr) {
            recordAllocation(pointer, static_cast<std::uint64_t>(requested_size));
        }
        return pointer;
    }

    void *handleCalloc(CallocFn function, std::size_t count, std::size_t requested_size)
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            return last_error.call(function, count, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return last_error.call(function, count, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return last_error.call(function, count, requested_size);
        }
        void *pointer = last_error.call(function, count, requested_size);
        if (pointer != nullptr) {
            std::uint64_t bytes = 0;
            if (checkedMultiply(count, requested_size, bytes)) {
                recordAllocation(pointer, bytes);
            }
        }
        return pointer;
    }

    void *handleRealloc(ReallocFn function, void *pointer, std::size_t requested_size)
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            return last_error.call(function, pointer, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return last_error.call(function, pointer, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return last_error.call(function, pointer, requested_size);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        DetachedAllocationRollback rollback(*this, previous);
        void *new_pointer = last_error.call(function, pointer, requested_size);
        rollback.disarm();
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && requested_size == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && requested_size != 0) {
            // Allocation weights use successful requested bytes, not usable heap size.
            recordAllocation(new_pointer, static_cast<std::uint64_t>(requested_size));
        }
        return new_pointer;
    }

    void *handleRecalloc(RecallocFn function, void *pointer, std::size_t count, std::size_t requested_size)
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            return last_error.call(function, pointer, count, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return last_error.call(function, pointer, count, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return last_error.call(function, pointer, count, requested_size);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, requested_size, bytes);
        LiveAllocation *previous = detachAllocation(pointer);
        DetachedAllocationRollback rollback(*this, previous);
        void *new_pointer = last_error.call(function, pointer, count, requested_size);
        rollback.disarm();
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && valid_size && bytes != 0) {
            recordAllocation(new_pointer, bytes);
        }
        return new_pointer;
    }

    void *handleAlignedMalloc(AlignedMallocFn function, std::size_t size, std::size_t alignment)
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            return last_error.call(function, size, alignment);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return last_error.call(function, size, alignment);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return last_error.call(function, size, alignment);
        }
        void *pointer = last_error.call(function, size, alignment);
        if (pointer != nullptr) {
            recordAllocation(pointer, static_cast<std::uint64_t>(size));
        }
        return pointer;
    }

    void *handleAlignedRealloc(AlignedReallocFn function, void *pointer, std::size_t size, std::size_t alignment)
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            return last_error.call(function, pointer, size, alignment);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return last_error.call(function, pointer, size, alignment);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return last_error.call(function, pointer, size, alignment);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        DetachedAllocationRollback rollback(*this, previous);
        void *new_pointer = last_error.call(function, pointer, size, alignment);
        rollback.disarm();
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && size == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && size != 0) {
            recordAllocation(new_pointer, static_cast<std::uint64_t>(size));
        }
        return new_pointer;
    }

    void *handleAlignedRecalloc(AlignedRecallocFn function, void *pointer, std::size_t count, std::size_t size,
                                std::size_t alignment)
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            return last_error.call(function, pointer, count, size, alignment);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return last_error.call(function, pointer, count, size, alignment);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return last_error.call(function, pointer, count, size, alignment);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, size, bytes);
        LiveAllocation *previous = detachAllocation(pointer);
        DetachedAllocationRollback rollback(*this, previous);
        void *new_pointer = last_error.call(function, pointer, count, size, alignment);
        rollback.disarm();
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && valid_size && bytes != 0) {
            recordAllocation(new_pointer, bytes);
        }
        return new_pointer;
    }

    void *handleAlignedOffsetMalloc(AlignedOffsetMallocFn function, std::size_t size, std::size_t alignment,
                                    std::size_t offset)
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            return last_error.call(function, size, alignment, offset);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return last_error.call(function, size, alignment, offset);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return last_error.call(function, size, alignment, offset);
        }
        void *pointer = last_error.call(function, size, alignment, offset);
        if (pointer != nullptr) {
            recordAllocation(pointer, static_cast<std::uint64_t>(size));
        }
        return pointer;
    }

    void *handleAlignedOffsetRealloc(AlignedOffsetReallocFn function, void *pointer, std::size_t size,
                                     std::size_t alignment, std::size_t offset)
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            return last_error.call(function, pointer, size, alignment, offset);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return last_error.call(function, pointer, size, alignment, offset);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return last_error.call(function, pointer, size, alignment, offset);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        DetachedAllocationRollback rollback(*this, previous);
        void *new_pointer = last_error.call(function, pointer, size, alignment, offset);
        rollback.disarm();
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && size == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && size != 0) {
            recordAllocation(new_pointer, static_cast<std::uint64_t>(size));
        }
        return new_pointer;
    }

    void *handleAlignedOffsetRecalloc(AlignedOffsetRecallocFn function, void *pointer, std::size_t count,
                                      std::size_t size, std::size_t alignment, std::size_t offset)
    {
        AllocatorLastErrorGuard last_error;
        if (!shouldTrackCurrentThread()) {
            return last_error.call(function, pointer, count, size, alignment, offset);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return last_error.call(function, pointer, count, size, alignment, offset);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return last_error.call(function, pointer, count, size, alignment, offset);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, size, bytes);
        LiveAllocation *previous = detachAllocation(pointer);
        DetachedAllocationRollback rollback(*this, previous);
        void *new_pointer = last_error.call(function, pointer, count, size, alignment, offset);
        rollback.disarm();
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && valid_size && bytes != 0) {
            recordAllocation(new_pointer, bytes);
        }
        return new_pointer;
    }

    void *handleHeapAlloc(HeapAllocFn function, HANDLE heap, DWORD flags, SIZE_T requested_size)
    {
        return executeHeapAlloc(this, function, heap, flags, requested_size, false);
    }

    void *handleHeapReAlloc(HeapReAllocFn function, HANDLE heap, DWORD flags, void *pointer, SIZE_T requested_size)
    {
        return executeHeapReAlloc(this, function, heap, flags, pointer, requested_size, false);
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
        LiveAllocation *replaced = nullptr;
        bool inserted = false;
        void *pointer = allocation->pointer;
        const std::uint64_t allocation_id = allocation->allocation_id;
        const std::uint64_t weight = allocation->weight_bytes;
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t shard = liveIndexShard(hash);
        if (config.force_live_lock_contention_for_testing || !::TryAcquireSRWLockExclusive(&live_index_locks[shard])) {
            insertion_contention_failures.fetch_add(1, std::memory_order_relaxed);
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        lifecycle_writers.fetch_add(1, std::memory_order_acq_rel);
        lifecycle_version.fetch_add(1, std::memory_order_release);
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
                break;
            }
        }
        if (!inserted && tombstone != KLiveIndexCapacity) {
            if (account_live) {
                accountLiveAllocation(weight);
            }
            publishEntry(live_index[tombstone], pointer, allocation_id, allocation);
            inserted = true;
        }
        if (!inserted) {
            exhausted_insertion_probe_failures.fetch_add(1, std::memory_order_relaxed);
        }
        lifecycle_version.fetch_add(1, std::memory_order_release);
        lifecycle_writers.fetch_sub(1, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&live_index_locks[shard]);

        if (replaced != nullptr) {
            // Pointer reuse retires lifecycle records missed by covered frees.
            retireAllocation(replaced, monotonicMs());
        }
        return inserted;
    }

    void recordAllocation(void *pointer, std::uint64_t requested_bytes) noexcept
    {
        HotCounters &counters = hotCountersForCurrentThread();
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
        const auto current_tid = static_cast<std::uint64_t>(::GetCurrentThreadId());
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

        const std::uint64_t sample_points = consumeSampledBytes(state, requested_bytes, interval);
        if (sample_points == 0) {
            return;
        }
        sampling_points.fetch_add(sample_points, std::memory_order_relaxed);

        // Each sampling point contributes one configured interval.
        const std::uint64_t weight = saturatingMultiply(sample_points, interval);

        PSLIST_ENTRY live_entry = ::InterlockedPopEntrySList(&free_live_allocations);
        if (live_entry == nullptr && lifecycle_readers.load(std::memory_order_acquire) == 0) {
            live_entry = ::InterlockedPopEntrySList(&deferred_live_allocations);
        }
        if (live_entry == nullptr) {
            record_pool_acquisition_failures.fetch_add(1, std::memory_order_relaxed);
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        auto *allocation = CONTAINING_RECORD(live_entry, LiveAllocation, entry);
        allocation->pointer = pointer;
        allocation->allocation_id = next_allocation_id.fetch_add(1, std::memory_order_relaxed);
        allocation->weight_bytes = weight;
        allocation->requested_bytes = requested_bytes;
        allocation->allocated_ms = monotonicMs();
        allocation->tick_id = current_tick.load(std::memory_order_relaxed);
        allocation->thread_id = thread.session_thread_id;
        allocation->os_thread_id = thread.os_thread_id;
        allocation->window = profiling_window::windowNow();
        allocation->depth = static_cast<std::uint16_t>(captureDynamicAwareStackBackTrace(
            KFramesToSkip, static_cast<ULONG>(KStackDepth), allocation->frames, nullptr));
        const std::uint64_t allocation_weight = allocation->weight_bytes;
        const std::uint64_t allocation_tick = allocation->tick_id;
        const std::uint64_t allocation_thread = allocation->thread_id;
        const std::uint64_t allocation_os_thread = allocation->os_thread_id;
        const std::int32_t allocation_window = allocation->window;
        const std::uint16_t allocation_depth = allocation->depth;
        const bool live_only = config.live_only;
        AllocationEvent snapshot{};
        snapshot.thread_id = allocation_thread;
        snapshot.os_thread_id = allocation_os_thread;
        snapshot.thread_observation = live_only;
        if (!live_only) {
            snapshot.weight_bytes = allocation_weight;
            snapshot.tick_id = allocation_tick;
            snapshot.window = allocation_window;
            snapshot.depth = allocation_depth;
            std::memcpy(static_cast<void *>(snapshot.frames), static_cast<const void *>(allocation->frames),
                        static_cast<std::size_t>(allocation_depth) * sizeof(void *));
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
                PSLIST_ENTRY observation_entry = ::InterlockedPopEntrySList(&free_events);
                if (observation_entry == nullptr) {
                    dropped_samples.fetch_add(1, std::memory_order_relaxed);
                    dropped_events.fetch_add(1, std::memory_order_relaxed);
                }
                else {
                    auto *observation = CONTAINING_RECORD(observation_entry, AllocationEvent, entry);
                    observation->thread_id = snapshot.thread_id;
                    observation->os_thread_id = snapshot.os_thread_id;
                    observation->depth = 0;
                    observation->thread_observation = true;
                    const std::uint64_t ready = ready_event_count.fetch_add(1, std::memory_order_relaxed) + 1;
                    std::uint64_t previous_high_water = ready_event_high_water.load(std::memory_order_relaxed);
                    while (previous_high_water < ready && !ready_event_high_water.compare_exchange_weak(
                                                              previous_high_water, ready, std::memory_order_relaxed)) {
                    }
                    ::InterlockedPushEntrySList(&ready_events, &observation->entry);
                    thread.identity_announced = true;
                }
            }
            return;
        }

        PSLIST_ENTRY entry = ::InterlockedPopEntrySList(&free_events);
        if (entry == nullptr) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            dropped_events.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto *event = CONTAINING_RECORD(entry, AllocationEvent, entry);
        event->weight_bytes = snapshot.weight_bytes;
        event->tick_id = snapshot.tick_id;
        event->thread_id = snapshot.thread_id;
        event->os_thread_id = snapshot.os_thread_id;
        event->window = snapshot.window;
        event->depth = snapshot.depth;
        event->thread_observation = snapshot.thread_observation;
        std::memcpy(static_cast<void *>(event->frames), static_cast<const void *>(snapshot.frames),
                    static_cast<std::size_t>(snapshot.depth) * sizeof(void *));
        const std::uint64_t ready = ready_event_count.fetch_add(1, std::memory_order_relaxed) + 1;
        std::uint64_t previous_high_water = ready_event_high_water.load(std::memory_order_relaxed);
        while (previous_high_water < ready &&
               !ready_event_high_water.compare_exchange_weak(previous_high_water, ready, std::memory_order_relaxed)) {
        }
        ::InterlockedPushEntrySList(&ready_events, &event->entry);
        enqueued_samples.fetch_add(1, std::memory_order_relaxed);
    }

    bool allocateEventPool(std::string &error)
    {
        ::InitializeSListHead(&free_events);
        ::InitializeSListHead(&ready_events);
        ::InitializeSListHead(&free_live_allocations);
        ::InitializeSListHead(&deferred_live_allocations);
        const std::size_t bytes = sizeof(AllocationEvent) * KEventCapacity;
        event_storage =
            static_cast<AllocationEvent *>(::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (event_storage == nullptr) {
            error = "VirtualAlloc for allocation sample buffer failed: " + std::to_string(::GetLastError());
            return false;
        }
        live_storage = static_cast<LiveAllocation *>(
            ::VirtualAlloc(nullptr, sizeof(LiveAllocation) * KEventCapacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        live_index = static_cast<LiveIndexEntry *>(::VirtualAlloc(nullptr, sizeof(LiveIndexEntry) * KLiveIndexCapacity,
                                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (live_storage == nullptr || live_index == nullptr) {
            error = "VirtualAlloc for allocation lifecycle tracking failed: " + std::to_string(::GetLastError());
            freeEventPool();
            return false;
        }
        for (std::size_t i = 0; i < KEventCapacity; ++i) {
            ::new (static_cast<void *>(&event_storage[i])) AllocationEvent{};
            ::InterlockedPushEntrySList(&free_events, &event_storage[i].entry);
            ::new (static_cast<void *>(&live_storage[i])) LiveAllocation{};
            ::InterlockedPushEntrySList(&free_live_allocations, &live_storage[i].entry);
        }
        return true;
    }

    void freeEventPool() noexcept
    {
        if (event_storage != nullptr) {
            ::VirtualFree(event_storage, 0, MEM_RELEASE);
            event_storage = nullptr;
        }
        if (live_storage != nullptr) {
            ::VirtualFree(live_storage, 0, MEM_RELEASE);
            live_storage = nullptr;
        }
        if (live_index != nullptr) {
            ::VirtualFree(live_index, 0, MEM_RELEASE);
            live_index = nullptr;
        }
        ::InitializeSListHead(&free_events);
        ::InitializeSListHead(&ready_events);
        ::InitializeSListHead(&free_live_allocations);
        ::InitializeSListHead(&deferred_live_allocations);
    }

    void recycleEvent(AllocationEvent *event) noexcept { ::InterlockedPushEntrySList(&free_events, &event->entry); }

    template <typename Function>
    bool registerExport(HMODULE module, const char *name, Function &function, void *hook, bool required,
                        std::string &error)
    {
        void *target = reinterpret_cast<void *>(::GetProcAddress(module, name));
        if (target == nullptr) {
            function = nullptr;
            hook_capabilities.push_back(
                {.name = name, .status = AllocationHookStatus::Missing, .detail = "export not found"});
            if (required) {
                error = std::string("required UCRT allocation export not found: ") + name;
                return false;
            }
            return true;
        }
        auto alias = std::find_if(registered_targets.begin(), registered_targets.end(),
                                  [target](const RegisteredHookTarget &entry) { return entry.address == target; });
        if (alias != registered_targets.end()) {
            // Some CRT exports are aliases for the same implementation address. The first
            // registered target owns the shared gateway, so duplicate registrations are skipped.
            function = nullptr;
            hook_capabilities.push_back({name, AllocationHookStatus::Alias, alias->export_name});
            return true;
        }

        function = reinterpret_cast<Function>(target);
        std::string backend_error;
        if (hooks == nullptr || !hooks->addTarget(target, hook, backend_error)) {
            const std::string failure = std::string("allocation IAT target registration failed for ") + name +
                                        (backend_error.empty() ? std::string{} : ": " + backend_error);
            if (!required) {
                function = nullptr;
                hook_capabilities.push_back(
                    {.name = name, .status = AllocationHookStatus::PrepareFailed, .detail = failure});
                return true;
            }
            hook_capabilities.push_back(
                {.name = name, .status = AllocationHookStatus::PrepareFailed, .detail = failure});
            error = failure;
            return false;
        }
        registered_targets.push_back({.address = target, .export_name = name});
        hook_capabilities.push_back({.name = name, .status = AllocationHookStatus::Active, .detail = {}});
        return true;
    }

    bool configureHooks(std::string &error)
    {
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (fixture_no_hooks_for_testing) {
            return true;
        }
#endif
        if (hooks_configured) {
            return true;
        }
        if (tls_index == TLS_OUT_OF_INDEXES) {
            tls_index = ::TlsAlloc();
            if (tls_index == TLS_OUT_OF_INDEXES) {
                error = "TlsAlloc for allocation thread state failed: " + std::to_string(::GetLastError());
                return false;
            }
        }

        HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
        if (ucrt == nullptr) {
            error = "ucrtbase.dll is not loaded";
            return false;
        }
        HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        if (kernel32 == nullptr) {
            error = "kernel32.dll is not loaded";
            return false;
        }

        try {
            hooks = std::make_unique<WindowsAllocationIatHooks>();
        }
        catch (...) {
            error = "could not allocate the Windows allocation IAT backend";
            return false;
        }

        const bool ok =
            registerExport(ucrt, "malloc", real_malloc, reinterpret_cast<void *>(&hookMalloc), true, error) &&
            registerExport(ucrt, "calloc", real_calloc, reinterpret_cast<void *>(&hookCalloc), true, error) &&
            registerExport(ucrt, "realloc", real_realloc, reinterpret_cast<void *>(&hookRealloc), true, error) &&
            registerExport(ucrt, "_recalloc", real_recalloc, reinterpret_cast<void *>(&hookRecalloc), false, error) &&
            registerExport(ucrt, "free", real_free, reinterpret_cast<void *>(&hookFree), true, error) &&
            registerExport(ucrt, "_aligned_malloc", real_aligned_malloc, reinterpret_cast<void *>(&hookAlignedMalloc),
                           false, error) &&
            registerExport(ucrt, "_aligned_realloc", real_aligned_realloc,
                           reinterpret_cast<void *>(&hookAlignedRealloc), false, error) &&
            registerExport(ucrt, "_aligned_recalloc", real_aligned_recalloc,
                           reinterpret_cast<void *>(&hookAlignedRecalloc), false, error) &&
            registerExport(ucrt, "_aligned_offset_malloc", real_aligned_offset_malloc,
                           reinterpret_cast<void *>(&hookAlignedOffsetMalloc), false, error) &&
            registerExport(ucrt, "_aligned_offset_realloc", real_aligned_offset_realloc,
                           reinterpret_cast<void *>(&hookAlignedOffsetRealloc), false, error) &&
            registerExport(ucrt, "_aligned_offset_recalloc", real_aligned_offset_recalloc,
                           reinterpret_cast<void *>(&hookAlignedOffsetRecalloc), false, error) &&
            registerExport(ucrt, "_aligned_free", real_aligned_free, reinterpret_cast<void *>(&hookAlignedFree), true,
                           error) &&
            registerExport(ucrt, "_malloc_base", real_malloc_base, reinterpret_cast<void *>(&hookMallocBase), false,
                           error) &&
            registerExport(ucrt, "_calloc_base", real_calloc_base, reinterpret_cast<void *>(&hookCallocBase), false,
                           error) &&
            registerExport(ucrt, "_realloc_base", real_realloc_base, reinterpret_cast<void *>(&hookReallocBase), false,
                           error) &&
            registerExport(ucrt, "_free_base", real_free_base, reinterpret_cast<void *>(&hookFreeBase), false, error) &&
            registerExport(kernel32, "HeapAlloc", real_heap_alloc, reinterpret_cast<void *>(&hookHeapAlloc), false,
                           error) &&
            registerExport(kernel32, "HeapReAlloc", real_heap_realloc, reinterpret_cast<void *>(&hookHeapReAlloc),
                           false, error) &&
            registerExport(kernel32, "HeapFree", real_heap_free, reinterpret_cast<void *>(&hookHeapFree), true, error);

        if (!ok) {
            hooks.reset();
            clearFunctionPointers();
            return false;
        }

        Impl *expected = nullptr;
        if (!mActiveInstance.compare_exchange_strong(expected, this, std::memory_order_release,
                                                     std::memory_order_relaxed) &&
            expected != this) {
            error = "another native allocation sampler backend is already active";
            hooks.reset();
            clearFunctionPointers();
            return false;
        }

        hooks_configured = true;
        return true;
    }

    bool installHooks(std::string &error)
    {
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (fixture_no_hooks_for_testing) {
            return true;
        }
#endif
        if (hooks_installed.load(std::memory_order_acquire)) {
            return true;
        }
        if (!hooks_configured || hooks == nullptr) {
            error = "Windows allocation IAT backend is not configured";
            return false;
        }
        if (!hooks->install(error)) {
            hooks_installed.store(hooks->installed(), std::memory_order_release);
            if (error.empty()) {
                error = hooks->lastError();
            }
            return false;
        }
        hooks_installed.store(true, std::memory_order_release);
        return true;
    }

    static bool anyActiveHookCalls() noexcept
    {
        return std::ranges::any_of(mActiveHookCalls, [](const HookCounter &counter) {
            return counter.value.load(std::memory_order_acquire) != 0;
        });
    }

    bool uninstallHooks(std::string &error)
    {
        if (!hooks_installed.load(std::memory_order_acquire)) {
            return true;
        }
        if (hooks == nullptr) {
            error = "Windows allocation IAT backend is unavailable during detach";
            return false;
        }

        // Permanent-IAT teardown closes handler admission, drains callbacks already
        // admitted into Spark, clears the handler, then restores only IAT slots still
        // owned by the gateway. The process-lifetime gateway remains safe after unload.
        if (!hooks->uninstall(error)) {
            if (error.empty()) {
                error = hooks->lastError();
            }
            return false;
        }
        hooks_installed.store(false, std::memory_order_release);
        return true;
    }

    bool destroyHooks(std::string &error)
    {
        if (hooks == nullptr) {
            releaseThreadStateRegistry();
            return true;
        }
        if (hooks_installed.load(std::memory_order_acquire)) {
            error = "cannot destroy the Windows allocation IAT backend while hooks are installed";
            return false;
        }

        hooks.reset();
        hooks_configured = false;
        Impl *expected = this;
        mActiveInstance.compare_exchange_strong(expected, nullptr, std::memory_order_release,
                                                std::memory_order_relaxed);
        clearFunctionPointers();
        releaseThreadStateRegistry();
        return true;
    }

    void releaseThreadStateRegistry() noexcept
    {
        if (tls_index != TLS_OUT_OF_INDEXES) {
            ::TlsFree(tls_index);
            tls_index = TLS_OUT_OF_INDEXES;
        }
        for (ThreadSamplingState &state : thread_states) {
            if (state.thread_handle != nullptr) {
                ::CloseHandle(state.thread_handle);
                state.thread_handle = nullptr;
            }
            state.teb.store(nullptr, std::memory_order_relaxed);
            state.registry_state.store(0, std::memory_order_relaxed);
        }
    }

    void clearFunctionPointers() noexcept
    {
        real_malloc = nullptr;
        real_calloc = nullptr;
        real_realloc = nullptr;
        real_recalloc = nullptr;
        real_free = nullptr;
        real_aligned_malloc = nullptr;
        real_aligned_realloc = nullptr;
        real_aligned_recalloc = nullptr;
        real_aligned_offset_malloc = nullptr;
        real_aligned_offset_realloc = nullptr;
        real_aligned_offset_recalloc = nullptr;
        real_aligned_free = nullptr;
        real_malloc_base = nullptr;
        real_calloc_base = nullptr;
        real_realloc_base = nullptr;
        real_free_base = nullptr;
        real_heap_alloc = nullptr;
        real_heap_realloc = nullptr;
        real_heap_free = nullptr;
        registered_targets.clear();
        hook_capabilities.clear();
    }

    void cacheModule(std::uintptr_t module_base, ModuleId module_id)
    {
        if (module_cache.size() < KMaxModuleCacheEntries) {
            const auto [unused, inserted] = module_cache.emplace(module_base, module_id);
            (void)unused;
            if (inserted) {
                module_cache_size.fetch_add(1, std::memory_order_release);
            }
        }
        else {
            module_cache_insertion_refusals.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void discoverMainImage() noexcept
    {
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        ++main_image_discoveries_for_testing;
        if (force_main_image_discovery_failure_for_testing) {
            return;
        }
#endif
        const HMODULE module = ::GetModuleHandleW(nullptr);
        MODULEINFO info{};
        if (module != nullptr && ::GetModuleInformation(::GetCurrentProcess(), module, &info, sizeof(info)) != 0 &&
            info.lpBaseOfDll == module) {
            main_image_range =
                MainImageRange::validated(reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), info.SizeOfImage);
        }
    }

    FrameKey frameKeyForAddress(std::uint64_t raw_address, std::string &module_path)
    {
        std::uintptr_t module_base = 0;
        if (main_image_range.contains(static_cast<std::uintptr_t>(raw_address))) {
            module_base = main_image_range.base;
        }
        else {
            MEMORY_BASIC_INFORMATION memory{};
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
            ++fallback_queries_for_testing;
#endif
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            if (::VirtualQuery(reinterpret_cast<void *>(static_cast<std::uintptr_t>(raw_address)), &memory,
                               sizeof(memory)) != 0) {
                module_base = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
            }
        }

        ModuleId module_id = kInvalidModule;
        auto cache = module_cache.find(module_base);
        if (cache != module_cache.end()) {
            module_cache_hits.fetch_add(1, std::memory_order_relaxed);
            module_id = cache->second;
            module_path = aggregation.modules().path(module_id);
        }
        else {
            module_cache_misses.fetch_add(1, std::memory_order_relaxed);
            char path[MAX_PATH]{};
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            const DWORD length = module_base != 0 ? ::GetModuleFileNameA(reinterpret_cast<HMODULE>(module_base), path,
                                                                         static_cast<DWORD>(sizeof(path)))
                                                  : 0;
            module_path = length > 0 ? std::string(path, length) : std::string("unknown");
            module_id =
                aggregation
                    .internFrame(module_path, module_base != 0 ? raw_address - module_base : raw_address, raw_address)
                    .module;
            cacheModule(module_base, module_id);
        }

        return FrameKey{.module = module_id,
                        .rva = module_base != 0 ? raw_address - module_base : raw_address,
                        .raw_address = raw_address};
    }

    bool buildSample(void *const *frames, std::uint16_t depth, std::uint64_t tick_id, std::uint64_t thread_id,
                     std::uint64_t os_thread_id, std::int32_t window, std::uint64_t weight, Sample &sample)
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

        bool leading = true;
        for (std::size_t i = 0; i < depth; ++i) {
            const auto raw = reinterpret_cast<std::uint64_t>(frames[i]);
            if (raw == 0) {
                continue;
            }
            std::string path;
            FrameKey key = frameKeyForAddress(raw, path);
            if (isSparkAllocationInstrumentation(path)) {
                continue;
            }
            if (leading && isLeadingAllocatorRuntime(path)) {
                continue;
            }
            leading = false;
            sample.frames.push_back(key);
        }

        if (sample.frames.empty()) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    bool buildSnapshotSample(void *const *frames, std::uint16_t depth, std::uint64_t tick_id, std::uint64_t thread_id,
                             std::uint64_t os_thread_id, std::int32_t window, std::uint64_t weight, Sample &sample)
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

        bool leading = true;
        for (std::size_t i = 0; i < depth; ++i) {
            const auto raw = reinterpret_cast<std::uint64_t>(frames[i]);
            if (raw == 0) {
                continue;
            }
            std::string path;
            FrameKey key = frameKeyForAddress(raw, path);
            if (isSparkAllocationInstrumentation(path)) {
                continue;
            }
            if (leading && isLeadingAllocatorRuntime(path)) {
                continue;
            }
            leading = false;
            sample.frames.push_back(key);
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

    void processEvent(AllocationEvent *event, DrainContext context)
    {
        const bool caller_final_drain = context == DrainContext::CallerFinal;
        if (event->thread_observation) {
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
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (force_process_event_failure_for_testing) {
            throw std::runtime_error("injected allocation event processing failure");
        }
#endif
        if (config.aggregator_per_event_delay_us_for_testing != 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(config.aggregator_per_event_delay_us_for_testing));
        }
        if (event->thread_observation) {
            aggregation.observeThread(event->thread_id, event->os_thread_id);
            return;
        }
        Sample sample;
        if (!buildSample(event->frames, event->depth, event->tick_id, event->thread_id, event->os_thread_id,
                         event->window, event->weight_bytes, sample)) {
            return;
        }

        (void)aggregation.processSample(std::move(sample));
    }

    void finalizeLiveProfile()
    {
        AccountingFailureScope failure_scope(*this);
        const std::uint64_t stopped_ms = monotonicMs();
        const std::uint64_t terminal = terminal_tick.load(std::memory_order_acquire);
        const std::uint64_t retained_total = live_samples.load(std::memory_order_relaxed);
        std::uint64_t total_age = 0;
        std::uint64_t maximum_age = 0;
        std::uint64_t visited = 0;
        bool walk_truncated = false;
        for (std::size_t i = 0; i < KLiveIndexCapacity; ++i) {
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
            if (force_retained_walk_budget_for_testing) {
                retained_walk_visits_for_testing.store(visited, std::memory_order_release);
            }
#endif
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
            const std::uint64_t forced_elapsed_ms = visited >= 2 ? KDrainBudgetMs : 0;
#endif
            const std::uint64_t elapsed_ms =
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
                force_retained_walk_budget_for_testing ? forced_elapsed_ms :
#endif
                                                       monotonicMs() - stopped_ms;
            if (elapsed_ms >= KDrainBudgetMs) {
                walk_truncated = true;
                break;
            }
            const LiveIndexEntry &entry = live_index[i];
            void *pointer = entryPointer(entry);
            LiveAllocation *entry_allocation = entryAllocation(entry);
            if (pointer == nullptr || pointer == tombstonePointer() || entry_allocation == nullptr) {
                continue;
            }
            ++visited;
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
            if (force_retained_walk_budget_for_testing) {
                retained_walk_visits_for_testing.store(visited, std::memory_order_release);
            }
#endif
            if (config.live_finalize_per_record_delay_us_for_testing != 0) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(config.live_finalize_per_record_delay_us_for_testing));
            }
            const LiveAllocation &allocation = *entry_allocation;
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
        if (walk_truncated) {
            const std::uint64_t skipped = retained_total > visited ? retained_total - visited : 0;
            retained_allocations_skipped.fetch_add(skipped, std::memory_order_relaxed);
            drain_truncated.fetch_add(skipped, std::memory_order_relaxed);
        }
        retained_age_ms_total.store(total_age, std::memory_order_relaxed);
        retained_age_ms_max.store(maximum_age, std::memory_order_relaxed);
    }

    enum class DrainMode {
        Bounded,      // stop and snapshot drains: the drain budgets apply and may truncate
        SteadyState,  // in-session drain: the stop flags alone end the pass
    };

    bool drainBudgetExhausted(std::uint64_t started_ms, std::uint64_t processed, DrainMode mode) const noexcept
    {
        if (drain_abort.load(std::memory_order_acquire)) {
            return true;
        }
        if (mode == DrainMode::SteadyState) {
            return false;
        }
        const std::uint64_t elapsed_ms =
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
            force_drain_deadline_for_testing ? KDrainBudgetMs :
#endif
                                             monotonicMs() - started_ms;
        return processed >= KDrainBudgetEvents || elapsed_ms >= KDrainBudgetMs;
    }

    bool drainStopped(std::uint64_t started_ms, std::uint64_t processed, bool abort_when_aggregator_stopped,
                      DrainMode mode) const noexcept
    {
        if (abort_when_aggregator_stopped && !aggregator_running.load(std::memory_order_acquire)) {
            return true;
        }
        return drainBudgetExhausted(started_ms, processed, mode);
    }

    void recycleReadyEvents(PSLIST_ENTRY list, DrainContext context) noexcept
    {
        const bool caller_final_drain = context == DrainContext::CallerFinal;
        std::uint64_t discarded = 0;
        while (list != nullptr) {
            PSLIST_ENTRY next = list->Next;
            auto *event = CONTAINING_RECORD(list, AllocationEvent, entry);
            ready_event_count.fetch_sub(1, std::memory_order_relaxed);
            if (event->thread_observation) {
                discarded_thread_observation_events.fetch_add(1, std::memory_order_relaxed);
                drain_truncated_thread_observation_events.fetch_add(1, std::memory_order_relaxed);
                if (caller_final_drain) {
                    caller_final_drain_thread_observation_events.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else {
                discarded_allocation_events.fetch_add(1, std::memory_order_relaxed);
                drain_truncated_allocation_events.fetch_add(1, std::memory_order_relaxed);
                if (caller_final_drain) {
                    caller_final_drain_allocation_events.fetch_add(1, std::memory_order_relaxed);
                }
            }
            recycleEvent(event);
            list = next;
            ++discarded;
        }
        drain_truncated.fetch_add(discarded, std::memory_order_relaxed);
    }

    void drainQueues(bool abort_when_aggregator_stopped, DrainMode mode, DrainContext context)
    {
        DrainElapsedScope elapsed(*this, context);
        const std::uint64_t started_ms = monotonicMs();
        std::uint64_t processed = 0;
        TickEvent tick;
        while (!drainStopped(started_ms, processed, abort_when_aggregator_stopped, mode) && ticks.dequeue(tick)) {
            processed_tick_events.fetch_add(1, std::memory_order_relaxed);
            if (context == DrainContext::CallerFinal) {
                caller_final_drain_tick_events.fetch_add(1, std::memory_order_relaxed);
            }
            aggregation.processTick(tick.tick_id, tick.mspt_ms);
            ++processed;
        }
        if (mode == DrainMode::Bounded && drainBudgetExhausted(started_ms, processed, mode)) {
            TickEvent stranded;
            if (ticks.dequeue(stranded)) {
                discarded_tick_events.fetch_add(1, std::memory_order_relaxed);
                drain_truncated_tick_events.fetch_add(1, std::memory_order_relaxed);
                if (context == DrainContext::CallerFinal) {
                    caller_final_drain_tick_events.fetch_add(1, std::memory_order_relaxed);
                }
                drain_truncated.fetch_add(1, std::memory_order_relaxed);
            }
        }

        PSLIST_ENTRY list = ::InterlockedFlushSList(&ready_events);
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (list != nullptr && event_processing_gate_for_testing != nullptr) {
            event_processing_gate_for_testing->entered.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!event_processing_gate_for_testing->release.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    event_processing_gate_for_testing->timed_out.store(true, std::memory_order_release);
                    break;
                }
                yieldLifecycleTest();
            }
        }
#endif
        while (!drainStopped(started_ms, processed, abort_when_aggregator_stopped, mode) && list != nullptr) {
            PSLIST_ENTRY next = list->Next;
            auto *event = CONTAINING_RECORD(list, AllocationEvent, entry);
            processEvent(event, context);
            ready_event_count.fetch_sub(1, std::memory_order_relaxed);
            recycleEvent(event);
            list = next;
            ++processed;
        }
        if (mode == DrainMode::SteadyState && !drain_abort.load(std::memory_order_acquire) && list != nullptr) {
            PSLIST_ENTRY reversed = nullptr;
            while (list != nullptr) {
                PSLIST_ENTRY next = list->Next;
                list->Next = reversed;
                reversed = list;
                list = next;
            }
            while (reversed != nullptr) {
                PSLIST_ENTRY next = reversed->Next;
                ::InterlockedPushEntrySList(&ready_events, reversed);
                reversed = next;
            }
            return;
        }
        recycleReadyEvents(list, context);
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
        std::scoped_lock tick_lock(tick_mutex);
        tick_admission_open.store(false, std::memory_order_release);
        if (!finalize_pending.exchange(true, std::memory_order_acq_rel)) {
            terminal_tick.store(current_tick.load(std::memory_order_relaxed), std::memory_order_release);
        }
    }

    void finishAggregationIfNeeded()
    {
        std::scoped_lock lock(aggregate_mutex);
        AccountingFailureScope failure_scope(*this);
        drainQueues(false, DrainMode::Bounded, DrainContext::CallerFinal);
        if (finalize_pending.load(std::memory_order_acquire) && !pending_finalized.load(std::memory_order_acquire)) {
            aggregation.finishPending(terminal_tick.load(std::memory_order_acquire));
            pending_finalized.store(true, std::memory_order_release);
        }
    }

#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
    bool readThreadCpuTimeNs(HANDLE thread, std::uint64_t &value) const noexcept
#else
    static bool readThreadCpuTimeNs(HANDLE thread, std::uint64_t &value) noexcept
#endif
    {
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (force_aggregator_cpu_read_failure_for_testing) {
            value = 0;
            return false;
        }
#endif
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        if (::GetThreadTimes(thread, &creation, &exit, &kernel, &user) == FALSE) {
            value = 0;
            return false;
        }
        std::uint64_t kernel_ns = 0;
        std::uint64_t user_ns = 0;
        if (!fileTimeToNanoseconds(kernel.dwHighDateTime, kernel.dwLowDateTime, kernel_ns) ||
            !fileTimeToNanoseconds(user.dwHighDateTime, user.dwLowDateTime, user_ns) ||
            kernel_ns > std::numeric_limits<std::uint64_t>::max() - user_ns) {
            value = 0;
            return false;
        }
        value = kernel_ns + user_ns;
        return true;
    }

    void finishAggregatorCpuMeasurement(std::uint64_t started_cpu_ns, bool started_ok) noexcept
    {
        std::uint64_t finished_cpu_ns = 0;
        if (!started_ok || !readThreadCpuTimeNs(::GetCurrentThread(), finished_cpu_ns)) {
            aggregator_cpu_valid.store(false, std::memory_order_release);
            aggregator_cpu_read_failure.store(true, std::memory_order_release);
            return;
        }
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (force_aggregator_cpu_zero_for_testing) {
            finished_cpu_ns = started_cpu_ns;
        }
#endif
        if (finished_cpu_ns < started_cpu_ns) {
            aggregator_cpu_valid.store(false, std::memory_order_release);
            aggregator_cpu_read_failure.store(true, std::memory_order_release);
            return;
        }
        aggregator_cpu_time_ns.store(finished_cpu_ns - started_cpu_ns, std::memory_order_release);
        aggregator_cpu_valid.store(true, std::memory_order_release);
    }

#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
    bool runFixtureCpuWork() const noexcept
    {
        test::AllocationFixtureCpuWorkControl *control = fixture_cpu_work_for_testing;
        if (control == nullptr) {
            return true;
        }
        std::uint64_t started_cpu_ns = 0;
        if (!readThreadCpuTimeNs(::GetCurrentThread(), started_cpu_ns)) {
            control->failed.store(true, std::memory_order_release);
            return false;
        }
        constexpr std::size_t batch_size = 65536;
        std::uint32_t value = 0x9e3779b9U;
        std::uint64_t checksum = 0;
        std::uint64_t iterations = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            if (control->request_cancel.load(std::memory_order_acquire)) {
                control->cancelled.store(true, std::memory_order_release);
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                control->failed.store(true, std::memory_order_release);
                return false;
            }
            for (std::size_t i = 0; i < batch_size; ++i) {
                value ^= value << 13;
                value ^= value >> 7;
                value ^= value << 17;
                checksum += static_cast<std::uint64_t>(value) + i;
            }
            iterations += batch_size;
            control->checksum.store(checksum, std::memory_order_release);
            control->iterations.store(iterations, std::memory_order_release);

            std::uint64_t current_cpu_ns = 0;
            if (!readThreadCpuTimeNs(::GetCurrentThread(), current_cpu_ns) || current_cpu_ns < started_cpu_ns) {
                control->failed.store(true, std::memory_order_release);
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                control->failed.store(true, std::memory_order_release);
                return false;
            }
            if (current_cpu_ns - started_cpu_ns >= control->target_cpu_ns) {
                break;
            }
        }
        if (control->request_cancel.load(std::memory_order_acquire)) {
            control->cancelled.store(true, std::memory_order_release);
            return false;
        }
        if (control->sleep_ms != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(control->sleep_ms));
        }
        control->done.store(true, std::memory_order_release);
        return true;
    }
#endif

    void aggregatorLoop()
    {
        ConsumerLifetimeScope lifetime(*this);
        std::uint64_t aggregator_cpu_started_ns = 0;
        const bool aggregator_cpu_started = readThreadCpuTimeNs(::GetCurrentThread(), aggregator_cpu_started_ns);
        if (!aggregator_cpu_started) {
            aggregator_cpu_read_failure.store(true, std::memory_order_release);
        }
        struct CpuMeasurementScope {
            Impl &impl;
            std::uint64_t started_ns;
            bool started_ok;
            ~CpuMeasurementScope() { impl.finishAggregatorCpuMeasurement(started_ns, started_ok); }
        } cpu_measurement{.impl = *this, .started_ns = aggregator_cpu_started_ns, .started_ok = aggregator_cpu_started};
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (fixture_worker_gate_for_testing != nullptr) {
            fixture_worker_gate_for_testing->entered.store(true, std::memory_order_release);
            if (!waitFixtureWorkerGate(*fixture_worker_gate_for_testing)) {
                throw std::runtime_error("allocation fixture worker synchronization failed");
            }
        }
#endif
        TrackingSuppressionGuard suppress(*this);
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (fixture_cpu_work_for_testing != nullptr) {
            if (!runFixtureCpuWork()) {
                if (!fixture_cpu_work_for_testing->request_cancel.load(std::memory_order_acquire)) {
                    throw std::runtime_error("allocation fixture CPU work failed");
                }
                return;
            }
        }
#endif
        if (config.fail_aggregator_for_testing) {
            throw std::runtime_error("injected allocation aggregator failure");
        }
        if (config.aggregator_delay_ms_for_testing != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config.aggregator_delay_ms_for_testing));
        }
        std::uint64_t next_hook_refresh_ms = monotonicMs() + KHookRefreshIntervalMs;
        while (aggregator_running.load(std::memory_order_acquire)) {
            const std::uint64_t now_ms = monotonicMs();
            if (!drain_abort.load(std::memory_order_acquire) && now_ms >= next_hook_refresh_ms) {
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
                if (!fixture_no_hooks_for_testing) {
#endif
                    std::string refresh_error;
                    if (hooks == nullptr || !hooks->refresh(refresh_error)) {
                        throw std::runtime_error(
                            std::string("allocation hook refresh failed: ") +
                            (refresh_error.empty() ? "unknown hook refresh failure" : refresh_error));
                    }
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
                }
#endif
                next_hook_refresh_ms = now_ms + KHookRefreshIntervalMs;
            }
            {
                std::scoped_lock lock(aggregate_mutex);
                drainQueues(true, DrainMode::SteadyState, DrainContext::Aggregator);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::scoped_lock lock(aggregate_mutex);
            drainQueues(false, DrainMode::Bounded, DrainContext::Aggregator);
            if (!drain_abort.load(std::memory_order_acquire) && finalize_pending.load(std::memory_order_acquire)) {
                aggregation.finishPending(terminal_tick.load(std::memory_order_acquire));
                pending_finalized.store(true, std::memory_order_release);
            }
        }
    }

    bool captureSnapshot(AllocationSnapshot &snapshot, std::string &error)
    {
        TrackingSuppressionGuard suppress(*this);
        AccountingFailureScope failure_scope(*this);
        std::scoped_lock lifecycle_lock(lifecycle_mutex);
        error.clear();
        if (!running.load(std::memory_order_acquire)) {
            return false;
        }
        if (aggregator_failed.load(std::memory_order_acquire)) {
            error = "allocation aggregator failed: " + std::string(aggregator_failure.data());
            return false;
        }

        std::unique_lock aggregate_lock(aggregate_mutex, std::defer_lock);
        if (!aggregate_lock.try_lock_for(std::chrono::seconds(5))) {
            error = "timed out waiting for the allocation aggregator snapshot";
            return false;
        }
        drainQueues(false, DrainMode::Bounded, DrainContext::CallerSnapshot);
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
                [this](std::size_t shard) { return ::TryAcquireSRWLockShared(&live_index_locks[shard]) != 0; },
                [this](std::size_t shard) { ::ReleaseSRWLockShared(&live_index_locks[shard]); },
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
        ThreadSamplingState *state = currentThreadState();
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
        std::snprintf(aggregator_failure.data(), aggregator_failure.size(), "%s",
                      message != nullptr ? message : "unknown allocation aggregator failure");
        aggregator_failed.store(true, std::memory_order_release);
    }

    void resetSession()
    {
        accounting_state.store(AllocationAccountingState::NotStarted, std::memory_order_release);
        TickEvent tick;
        while (ticks.dequeue(tick)) {
        }
        module_cache.clear();
        main_image_range = {};
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        main_image_discoveries_for_testing = 0;
        fallback_queries_for_testing = 0;
#endif
        current_tick.store(0, std::memory_order_relaxed);
        for (HotCounters &counters : hot_counters) {
            counters.hook_calls.store(0, std::memory_order_relaxed);
            counters.successful_allocation_calls.store(0, std::memory_order_relaxed);
            counters.observed_bytes.store(0, std::memory_order_relaxed);
            counters.tracking_hook_calls.store(0, std::memory_order_relaxed);
        }
        sampling_points.store(0, std::memory_order_relaxed);
        filtered_samples.store(0, std::memory_order_relaxed);
        dropped_samples.store(0, std::memory_order_relaxed);
        dropped_events.store(0, std::memory_order_relaxed);
        dropped_tick_events.store(0, std::memory_order_relaxed);
        enqueued_samples.store(0, std::memory_order_relaxed);
        ready_event_count.store(0, std::memory_order_relaxed);
        ready_event_high_water.store(0, std::memory_order_relaxed);
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
        tick_admission_open.store(true, std::memory_order_relaxed);
        finalize_pending.store(false, std::memory_order_relaxed);
        pending_finalized.store(false, std::memory_order_relaxed);
        terminal_tick.store(0, std::memory_order_relaxed);
        lifecycle_version.store(0, std::memory_order_relaxed);
        lifecycle_readers.store(0, std::memory_order_relaxed);
        lifecycle_writers.store(0, std::memory_order_relaxed);
        retained_age_ms_total.store(0, std::memory_order_relaxed);
        retained_age_ms_max.store(0, std::memory_order_relaxed);
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
        aggregator_cpu_valid.store(false, std::memory_order_relaxed);
        aggregator_cpu_read_failure.store(false, std::memory_order_relaxed);
        aggregator_cpu_time_ns.store(0, std::memory_order_relaxed);
        module_cache_hits.store(0, std::memory_order_relaxed);
        module_cache_misses.store(0, std::memory_order_relaxed);
        module_cache_insertion_refusals.store(0, std::memory_order_relaxed);
        module_cache_size.store(0, std::memory_order_relaxed);
        aggregator_failure.fill('\0');
        aggregator_failed.store(false, std::memory_order_release);
        drain_abort.store(false, std::memory_order_relaxed);
        aggregator_exited.store(false, std::memory_order_relaxed);
        stop_wait_timed_out.store(false, std::memory_order_relaxed);
        drain_truncated.store(0, std::memory_order_relaxed);
    }

    bool waitForTrackingQuiescence(std::string &error) noexcept
    {
        const bool quiesced = detail::waitForQuiescence<std::chrono::steady_clock>(
            std::chrono::seconds(5),
            [this] {
                return std::ranges::any_of(hot_counters, [](const HotCounters &counters) {
                    return counters.tracking_hook_calls.load(std::memory_order_acquire) != 0;
                });
            },
            [](std::chrono::steady_clock::duration) { ::Sleep(1); });
        if (quiesced) {
            return true;
        }
        try {
            error = "timed out waiting for allocation lifecycle hooks to quiesce";
        }
        catch (...) {
            error.clear();
        }
        return false;
    }

    bool aggregatorMayBeAlive() const noexcept { return aggregator_thread.joinable(); }

    bool backendCleanupPending() const noexcept
    {
        return backend_cleanup_pending.load(std::memory_order_acquire) ||
               backend_shutdown_pending.load(std::memory_order_acquire) || aggregatorMayBeAlive();
    }

    bool waitForAggregatorExit() noexcept
    {
        const std::uint64_t deadline = monotonicMs() + KAggregatorExitTimeoutMs;
        while (!aggregator_exited.load(std::memory_order_acquire)) {
            if (monotonicMs() >= deadline) {
                return false;
            }
            ::Sleep(1);
        }
        return true;
    }

    bool startSession(const AllocationSamplerConfig &new_config, std::string &error)
    {
        std::scoped_lock lock(lifecycle_mutex);
        error.clear();
        if (running.load(std::memory_order_acquire)) {
            error = "allocation profiler is already running";
            return false;
        }
        if (backendCleanupPending()) {
            error = "the previous allocation session has not finished cleanup";
            return false;
        }
        if (new_config.session_seed == 0) {
            error = "the allocation session seed is not available";
            return false;
        }
        if (new_config.interval_bytes <= 0) {
            error = "allocation sampling interval must be greater than zero";
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
        const std::uint64_t new_generation = generation.fetch_add(1, std::memory_order_relaxed) + 1;
        sampling_seed.store(new_generation ^ monotonicMs() ^ new_config.session_seed, std::memory_order_relaxed);

        if (!configureHooks(error)) {
            return false;
        }
        if (!new_config.count_only) {
            freeEventPool();
            if (!allocateEventPool(error)) {
                return false;
            }
            discoverMainImage();
        }

        if (!installHooks(error)) {
            freeEventPool();
            return false;
        }
        if (new_config.count_only) {
            accounting_state.store(AllocationAccountingState::NotApplicable, std::memory_order_release);
            running.store(true, std::memory_order_release);
            tracking.store(true, std::memory_order_release);
            start_attempt.dismiss();
            return true;
        }

        aggregator_running.store(true, std::memory_order_release);
        running.store(true, std::memory_order_release);
        accounting_state.store(AllocationAccountingState::Active, std::memory_order_release);
        tracking.store(true, std::memory_order_release);
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
        if (fixture_no_worker_for_testing) {
            aggregator_running.store(false, std::memory_order_release);
            start_attempt.dismiss();
            return true;
        }
#endif
        try {
            TrackingSuppressionGuard suppress(*this);
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
            if (start_failure_gate_for_testing != nullptr) {
                start_failure_gate_for_testing->before_thread_creation.store(true, std::memory_order_release);
                while (!start_failure_gate_for_testing->fail_now.load(std::memory_order_acquire)) {
                    yieldLifecycleTest();
                }
                throw AllocationThreadCreationFailureForTesting{};
            }
#endif
            aggregator_thread = std::thread([this]() {
                try {
                    aggregatorLoop();
                }
                catch (const std::exception &exception) {
                    markAggregatorFailure(exception.what());
                }
                catch (...) {
                    markAggregatorFailure("allocation aggregator failed with an unknown exception");
                }
                aggregator_exited.store(true, std::memory_order_release);
            });
        }
        catch (...) {
            backend_cleanup_pending.store(true, std::memory_order_release);
            requestFinalization();
            tracking.store(false, std::memory_order_release);
            running.store(false, std::memory_order_release);
            aggregator_running.store(false, std::memory_order_release);
            std::string quiescence_error;
            if (!waitForTrackingQuiescence(quiescence_error)) {
                error = std::move(quiescence_error);
                return false;
            }
            freeEventPool();
            backend_cleanup_pending.store(false, std::memory_order_release);
            error = "could not create the allocation aggregator thread";
            return false;
        }
        start_attempt.dismiss();
        return true;
    }

    bool stopSession(std::string &error)
    {
        std::scoped_lock lock(lifecycle_mutex);
        error.clear();
        if (!running.load(std::memory_order_acquire) && !backendCleanupPending()) {
            return true;
        }

        markStopIncomplete();
        backend_cleanup_pending.store(true, std::memory_order_release);
        requestFinalization();
        tracking.store(false, std::memory_order_release);
        running.store(false, std::memory_order_release);
        if (!waitForTrackingQuiescence(error)) {
            return false;
        }
        aggregator_running.store(false, std::memory_order_release);
        if (aggregatorMayBeAlive()) {
            if (!waitForAggregatorExit()) {
                drain_abort.store(true, std::memory_order_release);
                stop_wait_timed_out.store(true, std::memory_order_release);
                error = "timed out waiting for the allocation aggregator to stop; finalization skipped";
                return false;
            }
            if (aggregator_thread.joinable()) {
                aggregator_thread.join();
            }
        }
        finishAggregationIfNeeded();
        if (config.live_only && !aggregator_failed.load(std::memory_order_acquire)) {
            finalizeLiveProfile();
        }
        discardResidualTicks();
        freeEventPool();
        if (backend_shutdown_pending.load(std::memory_order_acquire)) {
            backend_cleanup_pending.store(false, std::memory_order_release);
            error = "allocation backend shutdown cleanup is pending";
            return false;
        }
        if (aggregator_failed.load(std::memory_order_acquire)) {
            error = "allocation aggregator failed: " + std::string(aggregator_failure.data());
            backend_cleanup_pending.store(false, std::memory_order_release);
            return false;
        }
        if (config.live_only && lifecycle_dropped.load(std::memory_order_relaxed) != 0) {
            error = "allocation lifecycle tracking lost records; retained profile discarded";
            backend_cleanup_pending.store(false, std::memory_order_release);
            return false;
        }
        backend_cleanup_pending.store(false, std::memory_order_release);
        if (!config.count_only) {
            publishComplete();
        }
        return true;
    }

    bool shutdownBackend(std::string &error)
    {
        std::scoped_lock lock(lifecycle_mutex);
        error.clear();

        const bool cleanup_needed = running.load(std::memory_order_acquire) || backendCleanupPending() ||
                                    hooks_installed.load(std::memory_order_acquire);
        backend_shutdown_pending.store(true, std::memory_order_release);
        if (cleanup_needed) {
            backend_cleanup_pending.store(true, std::memory_order_release);
        }
        if (cleanup_needed) {
            markStopIncomplete();
            requestFinalization();
        }
        tracking.store(false, std::memory_order_release);
        running.store(false, std::memory_order_release);
        if (!waitForTrackingQuiescence(error)) {
            return false;
        }
        drain_abort.store(true, std::memory_order_release);
        aggregator_running.store(false, std::memory_order_release);
        if (aggregatorMayBeAlive()) {
            if (!waitForAggregatorExit()) {
                stop_wait_timed_out.store(true, std::memory_order_release);
                error = "timed out waiting for the allocation aggregator to exit before shutdown";
                return false;
            }
            if (aggregator_thread.joinable()) {
                aggregator_thread.join();
            }
        }
        finishAggregationIfNeeded();
        discardResidualTicks();
        freeEventPool();

        if (hooks_installed.load(std::memory_order_acquire) && !uninstallHooks(error)) {
            return false;
        }

        if (!destroyHooks(error)) {
            return false;
        }
        backend_cleanup_pending.store(false, std::memory_order_release);
        backend_shutdown_pending.store(false, std::memory_order_release);
        return true;
    }

    void tick(double mspt_ms)
    {
        std::scoped_lock tick_lock(tick_mutex);
        if (!tick_admission_open.load(std::memory_order_acquire) || !running.load(std::memory_order_acquire) ||
            aggregator_failed.load(std::memory_order_acquire)) {
            return;
        }
        TrackingSuppressionGuard suppress(*this);
        const std::uint64_t finished = current_tick.fetch_add(1, std::memory_order_relaxed);
        if (config.only_ticks_over_ms > 0 && !ticks.enqueue(TickEvent{.tick_id = finished, .mspt_ms = mspt_ms})) {
            dropped_tick_events.fetch_add(1, std::memory_order_relaxed);
        }

        if (config.count_only) {
            if ((finished % 40U) == 0U && hooks != nullptr) {
                std::unique_lock lock(lifecycle_mutex, std::try_to_lock);
                if (lock.owns_lock()) {
                    std::string refresh_error;
                    if (!hooks->refresh(refresh_error)) {
                        markAggregatorFailure(refresh_error.empty() ? "allocation hook refresh failed"
                                                                    : refresh_error.c_str());
                        running.store(false, std::memory_order_release);
                    }
                }
            }
            return;
        }
        const std::int32_t window = profiling_window::windowNow();
        aggregation.recordTick(window, mspt_ms);
    }
};

std::atomic<AllocationSampler::Impl *> AllocationSampler::Impl::mActiveInstance{nullptr};

#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
namespace test {

bool AllocationDiagnosticsTestAccess::configureFixture(AllocationSampler &sampler, bool no_hooks, bool no_worker,
                                                       AllocationFixtureWorkerGate *worker_gate) noexcept
{
    if ((no_worker && worker_gate != nullptr) || (!no_worker && worker_gate == nullptr)) {
        return false;
    }
    if (sampler.impl_->running.load(std::memory_order_acquire) || sampler.impl_->backendCleanupPending() ||
        sampler.impl_->hooks_installed.load(std::memory_order_acquire) || sampler.impl_->event_storage != nullptr ||
        sampler.impl_->live_storage != nullptr || sampler.impl_->live_index != nullptr ||
        sampler.impl_->fixture_controls_configured_for_testing) {
        return false;
    }
    sampler.impl_->fixture_no_hooks_for_testing = no_hooks;
    sampler.impl_->fixture_no_worker_for_testing = no_worker;
    sampler.impl_->fixture_controls_configured_for_testing = true;
    sampler.impl_->fixture_worker_gate_for_testing = worker_gate;
    sampler.impl_->fixture_thread_state_active_for_testing = no_hooks && no_worker;
    sampler.impl_->fixture_thread_owner_for_testing =
        sampler.impl_->fixture_thread_state_active_for_testing ? static_cast<std::uint64_t>(::GetCurrentThreadId()) : 0;
    if (worker_gate != nullptr) {
        worker_gate->entered.store(false, std::memory_order_relaxed);
        worker_gate->release.store(false, std::memory_order_relaxed);
        worker_gate->timed_out.store(false, std::memory_order_relaxed);
    }
    sampler.impl_->fixture_thread_state_for_testing.registry_state.store(0, std::memory_order_relaxed);
    sampler.impl_->fixture_thread_state_for_testing.teb.store(nullptr, std::memory_order_relaxed);
    sampler.impl_->fixture_thread_state_for_testing.thread_handle = nullptr;
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
        sampler.impl_->hooks_installed.load(std::memory_order_acquire) || sampler.impl_->event_storage != nullptr ||
        sampler.impl_->live_storage != nullptr || sampler.impl_->live_index != nullptr ||
        sampler.impl_->aggregator_thread.joinable()) {
        return false;
    }
    sampler.impl_->fixture_worker_gate_for_testing = nullptr;
    sampler.impl_->fixture_cpu_work_for_testing = nullptr;
    sampler.impl_->event_processing_gate_for_testing = nullptr;
    sampler.impl_->start_failure_gate_for_testing = nullptr;
    sampler.impl_->force_process_event_failure_for_testing = false;
    sampler.impl_->force_main_image_discovery_failure_for_testing = false;
    sampler.impl_->fixture_no_hooks_for_testing = false;
    sampler.impl_->fixture_no_worker_for_testing = false;
    sampler.impl_->fixture_controls_configured_for_testing = false;
    sampler.impl_->fixture_thread_owner_for_testing = 0;
    sampler.impl_->fixture_thread_state_active_for_testing = false;
    return true;
}

bool AllocationDiagnosticsTestAccess::fixtureStorageReady(const AllocationSampler &sampler) noexcept
{
    return sampler.impl_->event_storage != nullptr && sampler.impl_->live_storage != nullptr &&
           sampler.impl_->live_index != nullptr;
}

bool AllocationDiagnosticsTestAccess::fixtureWorkerPresent(const AllocationSampler &sampler) noexcept
{
    return sampler.impl_->aggregator_thread.joinable();
}

bool AllocationDiagnosticsTestAccess::fixtureHooksPresent(const AllocationSampler &sampler) noexcept
{
    return sampler.impl_->hooks_installed.load(std::memory_order_acquire);
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

        constexpr std::size_t max_events = 5;
        const auto event_count =
            static_cast<std::size_t>(requested.allocation_events + requested.thread_observation_events);
        std::array<AllocationSampler::Impl::AllocationEvent *, max_events> events{};
        std::size_t acquired = 0;
        std::size_t published = 0;
        for (; acquired < event_count; ++acquired) {
            PSLIST_ENTRY entry = ::InterlockedPopEntrySList(&sampler.impl_->free_events);
            if (entry == nullptr) {
                for (std::size_t i = 0; i < acquired; ++i) {
                    ::InterlockedPushEntrySList(&sampler.impl_->free_events, &events[i]->entry);
                }
                return false;
            }
            events[acquired] = CONTAINING_RECORD(entry, AllocationSampler::Impl::AllocationEvent, entry);
        }
        for (std::size_t i = 0; i < event_count; ++i) {
            *events[i] = AllocationSampler::Impl::AllocationEvent{};
            events[i]->weight_bytes = 1;
            events[i]->tick_id = 0;
            events[i]->thread_id = 100 + i;
            events[i]->os_thread_id = 200 + i;
            events[i]->window = 0;
            events[i]->thread_observation = i >= requested.allocation_events;
            if (!events[i]->thread_observation) {
                events[i]->depth = 1;
                events[i]->frames[0] = reinterpret_cast<void *>(&diagnosticsTestFrameAnchor);
            }
        }

        for (; published < event_count; ++published) {
            if (!admitted()) {
                for (std::size_t i = published; i < acquired; ++i) {
                    ::InterlockedPushEntrySList(&sampler.impl_->free_events, &events[i]->entry);
                }
                return false;
            }
            auto *event = events[published];
            ::InterlockedPushEntrySList(&sampler.impl_->ready_events, &event->entry);
            const std::uint64_t ready = sampler.impl_->ready_event_count.fetch_add(1, std::memory_order_relaxed) + 1;
            std::uint64_t previous = sampler.impl_->ready_event_high_water.load(std::memory_order_relaxed);
            while (previous < ready && !sampler.impl_->ready_event_high_water.compare_exchange_weak(
                                           previous, ready, std::memory_order_relaxed)) {
            }
            if (event->thread_observation) {
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

void AllocationDiagnosticsTestAccess::forceAggregatorCpuReadFailure(AllocationSampler &sampler, bool force) noexcept
{
    sampler.impl_->force_aggregator_cpu_read_failure_for_testing = force;
}

void AllocationDiagnosticsTestAccess::forceAggregatorCpuZero(AllocationSampler &sampler, bool force) noexcept
{
    sampler.impl_->force_aggregator_cpu_zero_for_testing = force;
}

void AllocationDiagnosticsTestAccess::armEventProcessingGate(AllocationSampler &sampler,
                                                             AllocationEventProcessingGate &gate) noexcept
{
    gate.entered.store(false, std::memory_order_relaxed);
    gate.release.store(false, std::memory_order_relaxed);
    gate.timed_out.store(false, std::memory_order_relaxed);
    sampler.impl_->event_processing_gate_for_testing = &gate;
}

void AllocationDiagnosticsTestAccess::disarmEventProcessingGate(AllocationSampler &sampler) noexcept
{
    sampler.impl_->event_processing_gate_for_testing = nullptr;
}

void AllocationDiagnosticsTestAccess::forceProcessEventFailure(AllocationSampler &sampler, bool force) noexcept
{
    sampler.impl_->force_process_event_failure_for_testing = force;
}

bool AllocationDiagnosticsTestAccess::configureFixtureCpuWork(AllocationSampler &sampler,
                                                              AllocationFixtureCpuWorkControl &control) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        sampler.impl_->running.load(std::memory_order_acquire) || sampler.impl_->backendCleanupPending() ||
        !sampler.impl_->fixture_no_hooks_for_testing || sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->fixture_worker_gate_for_testing == nullptr || sampler.impl_->aggregator_thread.joinable() ||
        sampler.impl_->fixture_cpu_work_for_testing != nullptr || control.target_cpu_ns != 60'000'000 ||
        (control.sleep_ms != 0 && control.sleep_ms != 400)) {
        return false;
    }
    control.done.store(false, std::memory_order_relaxed);
    control.failed.store(false, std::memory_order_relaxed);
    control.cancelled.store(false, std::memory_order_relaxed);
    control.checksum.store(0, std::memory_order_relaxed);
    control.iterations.store(0, std::memory_order_relaxed);
    control.request_cancel.store(false, std::memory_order_relaxed);
    sampler.impl_->fixture_cpu_work_for_testing = &control;
    return true;
}

void AllocationDiagnosticsTestAccess::forceDrainDeadline(AllocationSampler &sampler, bool force) noexcept
{
    sampler.impl_->force_drain_deadline_for_testing = force;
}

void AllocationDiagnosticsTestAccess::forceRetainedWalkBudget(AllocationSampler &sampler, bool force) noexcept
{
    sampler.impl_->force_retained_walk_budget_for_testing = force;
    sampler.impl_->retained_walk_visits_for_testing.store(0, std::memory_order_release);
}

std::uint64_t AllocationDiagnosticsTestAccess::retainedWalkVisits(const AllocationSampler &sampler) noexcept
{
    return sampler.impl_->retained_walk_visits_for_testing.load(std::memory_order_acquire);
}

bool AllocationDiagnosticsTestAccess::drainFixtureAggregatorContext(AllocationSampler &sampler) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->event_storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr) {
        return false;
    }
    std::unique_lock lock(sampler.impl_->aggregate_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(5))) {
        return false;
    }
    sampler.impl_->drainQueues(false, AllocationSampler::Impl::DrainMode::Bounded,
                               AllocationSampler::Impl::DrainContext::Aggregator);
    return true;
}

bool AllocationDiagnosticsTestAccess::liveRecordState(AllocationSampler &sampler, void *pointer,
                                                      AllocationLiveRecordState &result) noexcept
{
    result = {};
    if (pointer == nullptr || !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        sampler.impl_->live_index == nullptr) {
        return false;
    }
    const std::uint64_t hash = AllocationSampler::Impl::liveIndexHash(pointer);
    const std::size_t shard = AllocationSampler::Impl::liveIndexShard(hash);
    if (!::TryAcquireSRWLockShared(&sampler.impl_->live_index_locks[shard])) {
        return false;
    }
    bool valid = true;
    for (std::size_t offset = 0; offset < KLiveIndexShardCapacity; ++offset) {
        const auto &entry = sampler.impl_->live_index[AllocationSampler::Impl::liveIndexSlot(hash, shard, offset)];
        const void *entry_pointer = AllocationSampler::Impl::entryPointer(entry);
        if (entry_pointer == nullptr) {
            break;
        }
        if (entry_pointer == tombstonePointer() || entry_pointer != pointer) {
            continue;
        }
        AllocationSampler::Impl::LiveAllocation *record = AllocationSampler::Impl::entryAllocation(entry);
        if (record == nullptr || AllocationSampler::Impl::entryAllocationId(entry) != record->allocation_id ||
            record->pointer != pointer) {
            valid = false;
            break;
        }
        result.allocation_id = record->allocation_id;
        result.requested_bytes = record->requested_bytes;
        result.weight_bytes = record->weight_bytes;
        result.found = true;
        break;
    }
    ::ReleaseSRWLockShared(&sampler.impl_->live_index_locks[shard]);
    return valid;
}

bool AllocationDiagnosticsTestAccess::seedFixtureLiveAllocations(AllocationSampler &sampler, void *const *pointers,
                                                                 std::size_t count) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->event_storage == nullptr ||
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
            PSLIST_ENTRY entry = ::InterlockedPopEntrySList(&sampler.impl_->free_live_allocations);
            if (entry == nullptr) {
                throw std::runtime_error("fixture live record pool exhausted");
            }
            auto *record = CONTAINING_RECORD(entry, AllocationSampler::Impl::LiveAllocation, entry);
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
            record->frames[0] = reinterpret_cast<void *>(&diagnosticsTestFrameAnchor);
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
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->event_storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr || pointer == nullptr ||
        requested_bytes == 0 || result.opaque != nullptr) {
        return false;
    }
    PSLIST_ENTRY entry = ::InterlockedPopEntrySList(&sampler.impl_->free_live_allocations);
    if (entry == nullptr) {
        return false;
    }
    auto *record = CONTAINING_RECORD(entry, AllocationSampler::Impl::LiveAllocation, entry);
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
    record->frames[0] = reinterpret_cast<void *>(&diagnosticsTestFrameAnchor);
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
    ::AcquireSRWLockExclusive(&sampler.impl_->live_index_locks[shard]);
    gate.ready.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!gate.release.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            gate.timed_out.store(true, std::memory_order_release);
            break;
        }
        yieldLifecycleTest();
    }
    ::ReleaseSRWLockExclusive(&sampler.impl_->live_index_locks[shard]);
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
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->event_storage == nullptr ||
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
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->event_storage == nullptr ||
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

bool AllocationDiagnosticsTestAccess::fileTimeToNanoseconds(std::uint32_t high, std::uint32_t low,
                                                            std::uint64_t &value) noexcept
{
    return spark::fileTimeToNanoseconds(high, low, value);
}

bool AllocationDiagnosticsTestAccess::mainImageRangeContains(std::uintptr_t base, std::uintptr_t size,
                                                             std::uintptr_t address) noexcept
{
    return MainImageRange::validated(base, size).contains(address);
}

bool AllocationDiagnosticsTestAccess::mainImageState(AllocationSampler &sampler,
                                                     AllocationMainImageState &state) noexcept
{
    try {
        std::scoped_lock lifecycle_lock(sampler.impl_->lifecycle_mutex);
        if (!sampler.impl_->fixture_controls_configured_for_testing || !sampler.impl_->fixture_no_hooks_for_testing ||
            !sampler.impl_->fixture_no_worker_for_testing || sampler.impl_->aggregator_thread.joinable() ||
            !sampler.impl_->running.load(std::memory_order_acquire)) {
            return false;
        }
        std::scoped_lock aggregate_lock(sampler.impl_->aggregate_mutex);
        state = {.base = sampler.impl_->main_image_range.base,
                 .size = sampler.impl_->main_image_range.size,
                 .discoveries = sampler.impl_->main_image_discoveries_for_testing,
                 .fallback_queries = sampler.impl_->fallback_queries_for_testing};
        return true;
    }
    catch (...) {
        return false;
    }
}

bool AllocationDiagnosticsTestAccess::forceMainImageDiscoveryFailure(AllocationSampler &sampler, bool force) noexcept
{
    try {
        std::scoped_lock lifecycle_lock(sampler.impl_->lifecycle_mutex);
        if (!sampler.impl_->fixture_controls_configured_for_testing || !sampler.impl_->fixture_no_hooks_for_testing ||
            !sampler.impl_->fixture_no_worker_for_testing || sampler.impl_->aggregator_thread.joinable() ||
            sampler.impl_->running.load(std::memory_order_acquire) || sampler.impl_->backendCleanupPending() ||
            sampler.impl_->hooks_installed.load(std::memory_order_acquire) || sampler.impl_->event_storage != nullptr ||
            sampler.impl_->live_storage != nullptr || sampler.impl_->live_index != nullptr) {
            return false;
        }
        std::scoped_lock aggregate_lock(sampler.impl_->aggregate_mutex);
        sampler.impl_->force_main_image_discovery_failure_for_testing = force;
        return true;
    }
    catch (...) {
        return false;
    }
}

bool AllocationDiagnosticsTestAccess::resolveFrame(AllocationSampler &sampler, std::uintptr_t address,
                                                   bool force_fallback, AllocationResolvedFrame &frame) noexcept
{
    try {
        std::scoped_lock lifecycle_lock(sampler.impl_->lifecycle_mutex);
        if (!sampler.impl_->fixture_controls_configured_for_testing || !sampler.impl_->fixture_no_hooks_for_testing ||
            !sampler.impl_->fixture_no_worker_for_testing || sampler.impl_->aggregator_thread.joinable() ||
            !sampler.impl_->running.load(std::memory_order_acquire) ||
            sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
            sampler.impl_->event_storage == nullptr || sampler.impl_->live_storage == nullptr ||
            sampler.impl_->live_index == nullptr) {
            return false;
        }
        std::scoped_lock aggregate_lock(sampler.impl_->aggregate_mutex);
        struct RangeRestore {
            MainImageRange &range;
            MainImageRange saved;
            ~RangeRestore() { range = saved; }
        } restore{.range = sampler.impl_->main_image_range, .saved = sampler.impl_->main_image_range};
        if (force_fallback) {
            sampler.impl_->main_image_range = {};
        }
        const FrameKey key = sampler.impl_->frameKeyForAddress(address, frame.path);
        frame.module = key.module;
        frame.rva = key.rva;
        frame.raw_address = key.raw_address;
        return true;
    }
    catch (...) {
        return false;
    }
}

void AllocationDiagnosticsTestAccess::seedModuleCache(AllocationSampler &sampler, std::size_t entries) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->event_storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr) {
        return;
    }
    try {
        for (std::size_t i = 0; i < entries; ++i) {
            sampler.impl_->cacheModule(static_cast<std::uintptr_t>(i + 1), kInvalidModule);
        }
    }
    catch (...) {
        return;
    }
}

bool AllocationDiagnosticsTestAccess::resolveFrameOnce(AllocationSampler &sampler) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->event_storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr) {
        return false;
    }
    try {
        std::string first_path;
        const auto address = reinterpret_cast<std::uint64_t>(&diagnosticsTestFrameAnchor);
        (void)sampler.impl_->frameKeyForAddress(address, first_path);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool AllocationDiagnosticsTestAccess::resolveFrameTwice(AllocationSampler &sampler) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->event_storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr) {
        return false;
    }
    try {
        std::string first_path;
        std::string second_path;
        const auto address = reinterpret_cast<std::uint64_t>(&diagnosticsTestFrameAnchor);
        (void)sampler.impl_->frameKeyForAddress(address, first_path);
        (void)sampler.impl_->frameKeyForAddress(address, second_path);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool AllocationDiagnosticsTestAccess::resolveMissingFrame(AllocationSampler &sampler) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->event_storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr) {
        return false;
    }
    try {
        std::string path;
        (void)sampler.impl_->frameKeyForAddress(std::numeric_limits<std::uint64_t>::max(), path);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool AllocationDiagnosticsTestAccess::exerciseRecordPoolEmpty(AllocationSampler &sampler, void *pointer) noexcept
{
    if (!sampler.impl_->fixture_controls_configured_for_testing ||
        !sampler.impl_->running.load(std::memory_order_acquire) ||
        sampler.impl_->accounting_state.load(std::memory_order_acquire) != AllocationAccountingState::Active ||
        !sampler.impl_->fixture_no_hooks_for_testing || !sampler.impl_->fixture_no_worker_for_testing ||
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->event_storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr || pointer == nullptr) {
        return false;
    }
    PSLIST_ENTRY held_free = ::InterlockedFlushSList(&sampler.impl_->free_live_allocations);
    PSLIST_ENTRY held_deferred = ::InterlockedFlushSList(&sampler.impl_->deferred_live_allocations);
    sampler.impl_->recordAllocation(pointer, 1);
    const auto restore = [](SLIST_HEADER &destination, PSLIST_ENTRY list) noexcept {
        PSLIST_ENTRY reversed = nullptr;
        while (list != nullptr) {
            PSLIST_ENTRY next = list->Next;
            list->Next = reversed;
            reversed = list;
            list = next;
        }
        while (reversed != nullptr) {
            PSLIST_ENTRY next = reversed->Next;
            ::InterlockedPushEntrySList(&destination, reversed);
            reversed = next;
        }
    };
    restore(sampler.impl_->free_live_allocations, held_free);
    restore(sampler.impl_->deferred_live_allocations, held_deferred);
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
        sampler.impl_->aggregator_thread.joinable() || sampler.impl_->event_storage == nullptr ||
        sampler.impl_->live_storage == nullptr || sampler.impl_->live_index == nullptr || backing == nullptr ||
        backing_bytes < fixture_count * pointer_stride) {
        return false;
    }

    try {
        std::array<AllocationSampler::Impl::LiveAllocation *, fixture_count> records{};
        std::size_t successful = 0;
        bool final_failed = false;
        for (std::size_t i = 0; i < fixture_count; ++i) {
            PSLIST_ENTRY live_entry = ::InterlockedPopEntrySList(&sampler.impl_->free_live_allocations);
            AllocationSampler::Impl::LiveAllocation *record =
                live_entry == nullptr ? nullptr
                                      : CONTAINING_RECORD(live_entry, AllocationSampler::Impl::LiveAllocation, entry);
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
            record->frames[0] = reinterpret_cast<void *>(&diagnosticsTestFrameAnchor);
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
std::array<AllocationSampler::Impl::HookCounter, 64> AllocationSampler::Impl::mActiveHookCalls{};

AllocationSampler::AllocationSampler() : impl_(std::make_unique<Impl>())
{
#ifdef SPARK_ALLOCATION_LAST_ERROR_TESTING
    test::runWindowsAllocationLastErrorTests(*impl_);
#endif
}

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
        std::fprintf(stderr, "[spark] allocation sampler shutdown failed: %s\n", error.c_str());
    }
    // Unsafe hook state cannot survive plugin unload.
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
    impl_->markStopIncomplete();
    if (impl_->running.load(std::memory_order_acquire) ||
        impl_->backend_cleanup_pending.load(std::memory_order_acquire)) {
        impl_->backend_cleanup_pending.store(true, std::memory_order_release);
    }
    impl_->tick_admission_open.store(false, std::memory_order_release);
    impl_->tracking.store(false, std::memory_order_release);
    impl_->running.store(false, std::memory_order_release);
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
    result.aggregator_cpu_supported = impl_->aggregator_cpu_supported.load(std::memory_order_acquire);
    result.aggregator_cpu_valid = impl_->aggregator_cpu_valid.load(std::memory_order_acquire);
    result.aggregator_cpu_read_failure = impl_->aggregator_cpu_read_failure.load(std::memory_order_acquire);
    result.aggregator_cpu_time_ns = impl_->aggregator_cpu_time_ns.load(std::memory_order_relaxed);
    result.module_cache_supported = true;
    result.module_cache_hits = impl_->module_cache_hits.load(std::memory_order_relaxed);
    result.module_cache_misses = impl_->module_cache_misses.load(std::memory_order_relaxed);
    result.module_cache_insertion_refusals = impl_->module_cache_insertion_refusals.load(std::memory_order_relaxed);
    result.module_cache_size = impl_->module_cache_size.load(std::memory_order_acquire);
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
    return impl_->ready_event_high_water.load(std::memory_order_relaxed);
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
    return KMaxModuleCacheEntries;
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
    return impl_->hooks_configured ? 2 : 0;
}

std::uint64_t AllocationSampler::skippedModuleCount() const  // NOLINT(readability-convert-member-functions-to-static)
{
    return 0;
}

std::uint64_t AllocationSampler::failedModuleCount() const  // NOLINT(readability-convert-member-functions-to-static)
{
    return 0;
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
           impl_->drain_truncated.load(std::memory_order_relaxed) != 0 ||
           impl_->stop_wait_timed_out.load(std::memory_order_acquire) ||
           impl_->aggregation.threadIdentityCacheDrops() != 0 || impl_->aggregation.dataIncomplete();
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

std::uint64_t AllocationSampler::drainTruncated() const
{
    return impl_->drain_truncated.load(std::memory_order_relaxed);
}

bool AllocationSampler::stopWaitTimedOut() const
{
    return impl_->stop_wait_timed_out.load(std::memory_order_acquire);
}

bool AllocationSampler::aggregatorMayBeAlive() const
{
    return impl_->aggregatorMayBeAlive();
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
    return impl_->running.load(std::memory_order_acquire);
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
    error = impl_->aggregator_failure.data();
    return true;
}

const char *AllocationSampler::backendId() noexcept
{
    return WindowsAllocationIatHooks::backendId();
}

const char *AllocationSampler::backendName() noexcept
{
    return WindowsAllocationIatHooks::backendName();
}

const std::vector<AllocationHookCapability> &AllocationSampler::hookCapabilities() const
{
    return impl_->hook_capabilities;
}

std::size_t AllocationSampler::hookTargetCount() const
{
    return impl_->registered_targets.size();
}

}  // namespace spark
