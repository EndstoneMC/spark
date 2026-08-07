#include "native/alloc/allocation_sampler.h"

#if !defined(__linux__) || !defined(__x86_64__)
#error "allocation_sampler_linux.cpp requires Linux x86-64"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cpptrace/cpptrace.hpp>

#include "native/alloc/bounded_event_queue.h"
#include "native/alloc/byte_sampler.h"
#include "native/alloc/allocation_thread_filter.h"
#include "native/alloc/elf_import_hooks.h"
#include "native/sampler/thread_info.h"

namespace spark {
namespace {

constexpr std::size_t kStackDepth = 48;
constexpr std::size_t kEventCapacity = 16384;
constexpr std::size_t kLiveIndexCapacity = kEventCapacity * 2;
constexpr std::size_t kLiveIndexShards = 64;
constexpr std::size_t kLiveIndexShardCapacity =
    kLiveIndexCapacity / kLiveIndexShards;
constexpr std::size_t kMaxSampledThreads = 256;
constexpr std::size_t kMaxAllocationModules = 512;
constexpr std::size_t kMaxProfileNodes = 131072;
constexpr std::size_t kMaxPendingSamples = 32768;
constexpr std::size_t kMaxTickDecisions = 100000;
constexpr std::size_t kTickEventCapacity = 4096;
constexpr std::int32_t kMaxProfileWindows = 86400;
constexpr std::size_t kFramesToSkip = 4;

std::uint64_t monotonicMs() noexcept
{
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000 +
           static_cast<std::uint64_t>(value.tv_nsec) / 1000000;
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
        cpptrace::frame_ptr frames[kStackDepth]{};
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
        cpptrace::frame_ptr frames[kStackDepth]{};
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
            const std::size_t bytes = sizeof(Cell) * kEventCapacity;
            void *memory = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (memory == MAP_FAILED) {
                storage = nullptr;
                error = "mmap for Linux allocation sample buffer failed: " +
                        std::string(std::strerror(errno));
                return false;
            }
            storage = static_cast<Cell *>(memory);
            for (std::size_t i = 0; i < kEventCapacity; ++i) {
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
                ::munmap(storage, sizeof(Cell) * kEventCapacity);
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
            for (;;) {
                cell = &storage[position & (kEventCapacity - 1)];
                const std::size_t sequence =
                    cell->sequence.load(std::memory_order_acquire);
                const std::intptr_t difference =
                    static_cast<std::intptr_t>(sequence) -
                    static_cast<std::intptr_t>(position);
                if (difference == 0) {
                    if (producer.compare_exchange_weak(
                            position, position + 1, std::memory_order_relaxed)) {
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
            const std::uint64_t current =
                size.fetch_add(1, std::memory_order_relaxed) + 1;
            std::uint64_t previous = high_water.load(std::memory_order_relaxed);
            while (previous < current &&
                   !high_water.compare_exchange_weak(
                       previous, current, std::memory_order_relaxed)) {
            }
            cell->event = event;
            cell->sequence.store(position + 1, std::memory_order_release);
            return true;
        }

        bool dequeue(AllocationEvent &event) noexcept
        {
            std::size_t position = consumer.load(std::memory_order_relaxed);
            Cell *cell = nullptr;
            for (;;) {
                cell = &storage[position & (kEventCapacity - 1)];
                const std::size_t sequence =
                    cell->sequence.load(std::memory_order_acquire);
                const std::intptr_t difference =
                    static_cast<std::intptr_t>(sequence) -
                    static_cast<std::intptr_t>(position + 1);
                if (difference == 0) {
                    if (consumer.compare_exchange_weak(
                            position, position + 1, std::memory_order_relaxed)) {
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
            event = cell->event;
            size.fetch_sub(1, std::memory_order_relaxed);
            cell->sequence.store(position + kEventCapacity,
                                 std::memory_order_release);
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

    class HookCallGuard {
    public:
        HookCallGuard() noexcept
        {
            active_hook_calls.fetch_add(1, std::memory_order_acq_rel);
        }
        ~HookCallGuard()
        {
            active_hook_calls.fetch_sub(1, std::memory_order_release);
        }
    };

    class TrackingCallGuard {
    public:
        explicit TrackingCallGuard(Impl &impl) noexcept : impl_(impl)
        {
            if (!impl_.tracking.load(std::memory_order_acquire)) {
                return;
            }
            impl_.tracking_calls.fetch_add(1, std::memory_order_acq_rel);
            if (impl_.tracking.load(std::memory_order_acquire)) {
                active_ = true;
            }
            else {
                impl_.tracking_calls.fetch_sub(1, std::memory_order_release);
            }
        }
        ~TrackingCallGuard()
        {
            if (active_) {
                impl_.tracking_calls.fetch_sub(1, std::memory_order_release);
            }
        }
        explicit operator bool() const noexcept { return active_; }

    private:
        Impl &impl_;
        bool active_ = false;
    };

    class RecursionGuard {
    public:
        explicit RecursionGuard(Impl &impl) noexcept
            : state_(impl.currentThreadState())
        {
            if (state_ != nullptr && !state_->inside_hook) {
                state_->inside_hook = true;
                owner_ = true;
            }
        }
        ~RecursionGuard()
        {
            if (owner_) {
                state_->inside_hook = false;
            }
        }
        bool owner() const noexcept { return owner_; }

    private:
        ThreadSamplingState *state_ = nullptr;
        bool owner_ = false;
    };

    class TrackingSuppressionGuard {
    public:
        explicit TrackingSuppressionGuard(Impl &impl) noexcept
            : state_(impl.currentThreadState())
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

    static std::atomic<Impl *> active_instance;
    static std::atomic<std::uint64_t> active_hook_calls;

    ElfImportHooks hooks;
    MallocFn real_malloc = nullptr;
    CallocFn real_calloc = nullptr;
    ReallocFn real_realloc = nullptr;
    FreeFn real_free = nullptr;
    ReallocArrayFn real_reallocarray = nullptr;
    AlignedAllocFn real_aligned_alloc = nullptr;
    PosixMemalignFn real_posix_memalign = nullptr;
    std::vector<AllocationHookCapability> hook_capabilities;

    std::mutex lifecycle_mutex;
    AllocationSamplerConfig config{};
    std::atomic<bool> tracking{false};
    std::atomic<bool> running{false};
    std::atomic<bool> aggregator_running{false};
    std::atomic<bool> aggregator_failed{false};
    std::array<char, 256> aggregator_failure{};
    std::atomic<std::uint64_t> tracking_calls{0};
    std::atomic<std::uint64_t> current_tick{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> interval_bytes{kDefaultAllocationIntervalBytes};
    std::atomic<std::uint64_t> sampling_seed{0};
    std::atomic<std::uint64_t> started_ms{0};
    std::atomic<std::uint64_t> hook_calls{0};
    std::atomic<std::uint64_t> successful_allocation_calls{0};
    std::atomic<std::uint64_t> sample_count{0};
    std::atomic<std::uint64_t> sampling_points{0};
    std::atomic<std::uint64_t> sampled_bytes{0};
    std::atomic<std::uint64_t> filtered_samples{0};
    std::atomic<std::uint64_t> observed_bytes{0};
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
    std::atomic<std::uint64_t> retained_age_ms_total{0};
    std::atomic<std::uint64_t> retained_age_ms_max{0};
    std::atomic<bool> profile_nodes_exhausted{false};
    std::uint64_t last_module_rescan_ms = 0;

    std::array<pthread_rwlock_t, kLiveIndexShards> live_index_locks{};
    pthread_mutex_t live_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_key_t thread_state_key{};
    bool thread_state_key_created = false;
    std::array<ThreadSamplingState, 2048> thread_states{};
    LiveAllocation *live_storage = nullptr;
    LiveAllocation *free_live = nullptr;
    LiveIndexEntry *live_index = nullptr;

    Impl()
    {
        for (pthread_rwlock_t &lock : live_index_locks) {
            ::pthread_rwlock_init(&lock, nullptr);
        }
    }

    ~Impl()
    {
        for (pthread_rwlock_t &lock : live_index_locks) {
            ::pthread_rwlock_destroy(&lock);
        }
        ::pthread_mutex_destroy(&live_pool_mutex);
    }

    EventQueue events;
    std::thread aggregator_thread;
    BoundedEventQueue<TickEvent, kTickEventCapacity> ticks;
    CallTree tree;
    std::map<std::uint64_t, ThreadCallTree> thread_trees;
    AllocationThreadFilter thread_filter{kMaxSampledThreads,
                                         kLiveIndexCapacity};
    ModuleTable modules;
    std::map<std::int32_t, WindowTickStats> window_ticks;
    std::unordered_map<std::uint64_t, std::vector<Sample>> buckets;
    std::size_t pending_samples = 0;
    std::size_t profile_nodes_remaining = kMaxProfileNodes;
    std::vector<std::uint8_t> tick_decisions;

    static Impl *activeOrAbort() noexcept
    {
        Impl *impl = active_instance.load(std::memory_order_acquire);
        if (impl == nullptr) {
            std::abort();
        }
        if (impl->tracking.load(std::memory_order_relaxed)) {
            impl->hook_calls.fetch_add(1, std::memory_order_relaxed);
        }
        return impl;
    }

    static void *hookMalloc(std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handleMalloc(size);
    }

    static void *hookCalloc(std::size_t count, std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handleCalloc(count, size);
    }

    static void *hookRealloc(void *pointer, std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handleRealloc(pointer, size);
    }

    static void hookFree(void *pointer) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        impl->handleFree(pointer);
    }

    static void *hookReallocArray(void *pointer, std::size_t count, std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handleReallocArray(pointer, count, size);
    }

    static void *hookAlignedAlloc(std::size_t alignment, std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handleAlignedAlloc(alignment, size);
    }

    static int hookPosixMemalign(void **result, std::size_t alignment, std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handlePosixMemalign(result, alignment, size);
    }

    static void releaseThreadState(void *value) noexcept
    {
        if (value == nullptr ||
            value == reinterpret_cast<void *>(static_cast<std::uintptr_t>(1))) {
            return;
        }
        auto *state = static_cast<ThreadSamplingState *>(value);
        state->inside_hook = false;
        state->tracking_suppressed = false;
        state->owner_tid.store(0, std::memory_order_relaxed);
        state->registry_state.store(0, std::memory_order_release);
    }

    ThreadSamplingState *currentThreadState() noexcept
    {
        if (!thread_state_key_created) {
            return nullptr;
        }
        const std::uint64_t tid =
            static_cast<std::uint64_t>(::syscall(SYS_gettid));
        void *value = ::pthread_getspecific(thread_state_key);
        if (value ==
            reinterpret_cast<void *>(static_cast<std::uintptr_t>(1))) {
            return nullptr;
        }
        auto *state = static_cast<ThreadSamplingState *>(value);
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(state);
        const std::uintptr_t begin =
            reinterpret_cast<std::uintptr_t>(thread_states.data());
        const std::size_t state_limit =
            config.thread_state_limit_for_testing == 0
                ? thread_states.size()
                : (std::min)(
                      thread_states.size(),
                      static_cast<std::size_t>(
                          config.thread_state_limit_for_testing));
        const std::uintptr_t end =
            begin + state_limit * sizeof(ThreadSamplingState);
        if (address >= begin && address < end &&
            state->registry_state.load(std::memory_order_acquire) != 0 &&
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
            if (candidate.registry_state.compare_exchange_strong(
                    expected, 1, std::memory_order_acq_rel)) {
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
        if (!tracking.load(std::memory_order_relaxed)) {
            return false;
        }
        auto *self = const_cast<Impl *>(this);
        ThreadSamplingState *state = self->currentThreadState();
        return state != nullptr && !state->tracking_suppressed;
    }

    static std::uint64_t liveIndexHash(void *pointer) noexcept
    {
        const auto value = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(pointer) >> 4);
        return value * 11400714819323198485ull;
    }

    static std::size_t liveIndexShard(std::uint64_t hash) noexcept
    {
        return static_cast<std::size_t>(hash & (kLiveIndexShards - 1));
    }

    static std::size_t liveIndexSlot(std::uint64_t hash, std::size_t shard,
                                     std::size_t offset = 0) noexcept
    {
        const std::size_t within =
            (static_cast<std::size_t>(hash >> 6) + offset) &
            (kLiveIndexShardCapacity - 1);
        return shard * kLiveIndexShardCapacity + within;
    }

    void retireAllocation(LiveAllocation *allocation, std::uint64_t released_ms) noexcept
    {
        freed_samples.fetch_add(1, std::memory_order_relaxed);
        freed_bytes.fetch_add(allocation->weight_bytes, std::memory_order_relaxed);
        live_samples.fetch_sub(1, std::memory_order_relaxed);
        live_bytes.fetch_sub(allocation->weight_bytes, std::memory_order_relaxed);
        const std::uint64_t lifetime = released_ms >= allocation->allocated_ms
                                           ? released_ms - allocation->allocated_ms
                                           : 0;
        lifetime_ms_total.fetch_add(lifetime, std::memory_order_relaxed);
        std::uint64_t previous = lifetime_ms_max.load(std::memory_order_relaxed);
        while (previous < lifetime &&
               !lifetime_ms_max.compare_exchange_weak(previous, lifetime,
                                                      std::memory_order_relaxed)) {
        }
        if (::pthread_mutex_lock(&live_pool_mutex) == 0) {
            allocation->next = free_live;
            free_live = allocation;
            ::pthread_mutex_unlock(&live_pool_mutex);
        }
        else {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    LiveAllocation *detachAllocation(void *pointer) noexcept
    {
        if (pointer == nullptr || live_index == nullptr) {
            return nullptr;
        }
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t shard = liveIndexShard(hash);
        if (::pthread_rwlock_wrlock(&live_index_locks[shard]) != 0) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        LiveAllocation *detached = nullptr;
        for (std::size_t offset = 0; offset < kLiveIndexShardCapacity; ++offset) {
            LiveIndexEntry &entry =
                live_index[liveIndexSlot(hash, shard, offset)];
            if (entry.pointer == nullptr) {
                break;
            }
            if (entry.pointer == pointer) {
                if (entry.allocation == nullptr ||
                    entry.allocation_id != entry.allocation->allocation_id) {
                    lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                detached = entry.allocation;
                entry.pointer =
                    reinterpret_cast<void *>(static_cast<std::uintptr_t>(1));
                entry.allocation_id = 0;
                entry.allocation = nullptr;
                break;
            }
        }
        ::pthread_rwlock_unlock(&live_index_locks[shard]);
        return detached;
    }

    void restoreDetachedAllocation(LiveAllocation *allocation) noexcept
    {
        if (allocation == nullptr) {
            return;
        }
        if (!insertLiveAllocation(allocation)) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            live_samples.fetch_sub(1, std::memory_order_relaxed);
            live_bytes.fetch_sub(allocation->weight_bytes,
                                 std::memory_order_relaxed);
            recycleLiveRecord(allocation);
        }
    }

    void handleFree(void *pointer) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            real_free(pointer);
            return;
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            real_free(pointer);
            return;
        }
        LiveAllocation *allocation = detachAllocation(pointer);
        real_free(pointer);
        if (allocation != nullptr) {
            retireAllocation(allocation, monotonicMs());
        }
    }

    void *handleMalloc(std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_malloc(size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return real_malloc(size);
        }
        void *result = real_malloc(size);
        if (result != nullptr) {
            recordAllocation(result, static_cast<std::uint64_t>(size));
        }
        return result;
    }

    void *handleCalloc(std::size_t count, std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_calloc(count, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return real_calloc(count, size);
        }
        void *result = real_calloc(count, size);
        std::uint64_t bytes = 0;
        if (result != nullptr && checkedMultiply(count, size, bytes)) {
            recordAllocation(result, bytes);
        }
        return result;
    }

    void *handleRealloc(void *pointer, std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_realloc(pointer, size);
        }
        TrackingCallGuard tracking_guard(*this);
        if (!tracking_guard) {
            return real_realloc(pointer, size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return real_realloc(pointer, size);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *result = real_realloc(pointer, size);
        const bool replaced =
            result != nullptr || (pointer != nullptr && size == 0);
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
        if (!shouldTrackCurrentThread()) {
            return real_reallocarray(pointer, count, size);
        }
        TrackingCallGuard tracking_guard(*this);
        if (!tracking_guard) {
            return real_reallocarray(pointer, count, size);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, size, bytes);
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return real_reallocarray(pointer, count, size);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *result = real_reallocarray(pointer, count, size);
        const bool replaced =
            result != nullptr ||
            (pointer != nullptr && valid_size && bytes == 0);
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
        if (!shouldTrackCurrentThread()) {
            return real_aligned_alloc(alignment, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return real_aligned_alloc(alignment, size);
        }
        void *result = real_aligned_alloc(alignment, size);
        if (result != nullptr) {
            recordAllocation(result, static_cast<std::uint64_t>(size));
        }
        return result;
    }

    int handlePosixMemalign(void **result_pointer, std::size_t alignment,
                            std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_posix_memalign(result_pointer, alignment, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return real_posix_memalign(result_pointer, alignment, size);
        }
        const int result = real_posix_memalign(result_pointer, alignment, size);
        if (result == 0 && result_pointer != nullptr && *result_pointer != nullptr) {
            recordAllocation(*result_pointer, static_cast<std::uint64_t>(size));
        }
        return result;
    }

    LiveAllocation *acquireLiveRecord() noexcept
    {
        if (::pthread_mutex_lock(&live_pool_mutex) != 0) {
            return nullptr;
        }
        LiveAllocation *record = free_live;
        if (record != nullptr) {
            free_live = record->next;
            record->next = nullptr;
        }
        ::pthread_mutex_unlock(&live_pool_mutex);
        return record;
    }

    bool insertLiveAllocation(LiveAllocation *allocation) noexcept
    {
        const std::uint64_t hash = liveIndexHash(allocation->pointer);
        const std::size_t shard = liveIndexShard(hash);
        if (::pthread_rwlock_wrlock(&live_index_locks[shard]) != 0) {
            return false;
        }
        LiveAllocation *replaced = nullptr;
        bool inserted = false;
        std::size_t tombstone = kLiveIndexCapacity;
        for (std::size_t offset = 0; offset < kLiveIndexShardCapacity; ++offset) {
            const std::size_t slot = liveIndexSlot(hash, shard, offset);
            LiveIndexEntry &entry = live_index[slot];
            if (entry.pointer == reinterpret_cast<void *>(static_cast<std::uintptr_t>(1))) {
                if (tombstone == kLiveIndexCapacity) {
                    tombstone = slot;
                }
                continue;
            }
            if (entry.pointer == allocation->pointer) {
                replaced = entry.allocation;
                entry = {allocation->pointer, allocation->allocation_id, allocation};
                inserted = true;
                break;
            }
            if (entry.pointer == nullptr) {
                LiveIndexEntry &destination =
                    live_index[tombstone != kLiveIndexCapacity ? tombstone : slot];
                destination = {allocation->pointer, allocation->allocation_id, allocation};
                inserted = true;
                break;
            }
        }
        if (!inserted && tombstone != kLiveIndexCapacity) {
            live_index[tombstone] =
                {allocation->pointer, allocation->allocation_id, allocation};
            inserted = true;
        }
        ::pthread_rwlock_unlock(&live_index_locks[shard]);

        if (replaced != nullptr) {
            // Reuse proves that the previous allocation ended even when its
            // deallocation bypassed a patched import slot.
            retireAllocation(replaced, monotonicMs());
        }
        return inserted;
    }

    void recycleLiveRecord(LiveAllocation *allocation) noexcept
    {
        if (::pthread_mutex_lock(&live_pool_mutex) != 0) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        allocation->next = free_live;
        free_live = allocation;
        ::pthread_mutex_unlock(&live_pool_mutex);
    }

    void recordAllocation(void *pointer, std::uint64_t requested_bytes) noexcept
    {
        successful_allocation_calls.fetch_add(1, std::memory_order_relaxed);
        if (requested_bytes == 0) {
            return;
        }
        observed_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);
        ThreadSamplingState *thread_pointer = currentThreadState();
        if (thread_pointer == nullptr) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ThreadSamplingState &thread = *thread_pointer;
        ByteSamplingState &state = thread.bytes;
        const std::uint64_t current_generation = generation.load(std::memory_order_relaxed);
        const std::uint64_t interval = interval_bytes.load(std::memory_order_relaxed);
        const std::uint64_t current_tid =
            static_cast<std::uint64_t>(::syscall(SYS_gettid));
        if (state.generation != current_generation) {
            resetByteSamplingState(
                state, current_generation,
                sampling_seed.load(std::memory_order_relaxed) ^ current_generation ^
                    current_tid,
                interval);
        }
        if (thread.identity_generation != current_generation) {
            thread.identity_generation = current_generation;
            thread.os_thread_id = current_tid;
            const std::uint64_t next =
                next_session_thread_id.fetch_add(1, std::memory_order_relaxed);
            thread.session_thread_id = next;
            thread.identity_announced = false;
            if (next <= kMaxSampledThreads) {
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
        const std::uint64_t started = started_ms.load(std::memory_order_relaxed);
        allocation->window = static_cast<std::int32_t>((std::min)(
            static_cast<std::uint64_t>(kMaxProfileWindows - 1),
            started != 0 && allocation->allocated_ms >= started
                ? (allocation->allocated_ms - started) / 1000
                : 0));
        allocation->depth = static_cast<std::uint16_t>(cpptrace::safe_generate_raw_trace(
            allocation->frames, kStackDepth, kFramesToSkip));
        if (allocation->depth == 0 || !insertLiveAllocation(allocation)) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            recycleLiveRecord(allocation);
            return;
        }
        const std::uint64_t current_live =
            live_samples.fetch_add(1, std::memory_order_relaxed) + 1;
        live_bytes.fetch_add(allocation->weight_bytes, std::memory_order_relaxed);
        std::uint64_t previous_peak = peak_live_samples.load(std::memory_order_relaxed);
        while (previous_peak < current_live &&
               !peak_live_samples.compare_exchange_weak(
                   previous_peak, current_live, std::memory_order_relaxed)) {
        }

        if (config.live_only) {
            if (!thread.identity_announced) {
                AllocationEvent observation;
                observation.thread_id = allocation->thread_id;
                observation.os_thread_id = allocation->os_thread_id;
                observation.thread_observation = true;
                if (events.enqueue(observation)) {
                    thread.identity_announced = true;
                }
                else {
                    dropped_samples.fetch_add(1, std::memory_order_relaxed);
                    dropped_events.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        AllocationEvent event;
        event.weight_bytes = allocation->weight_bytes;
        event.tick_id = allocation->tick_id;
        event.thread_id = allocation->thread_id;
        event.os_thread_id = allocation->os_thread_id;
        event.window = allocation->window;
        event.depth = allocation->depth;
        event.thread_observation = false;
        std::memcpy(event.frames, allocation->frames,
                    static_cast<std::size_t>(allocation->depth) *
                        sizeof(cpptrace::frame_ptr));
        if (!events.enqueue(event)) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            dropped_events.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        enqueued_samples.fetch_add(1, std::memory_order_relaxed);
    }

    template <typename Function>
    bool resolveAllocator(const char *name, Function &function, bool required,
                          std::string &error)
    {
        ::dlerror();
        // Resolve the process-wide effective allocator before import slots are
        // patched. RTLD_DEFAULT preserves LD_PRELOAD interposition instead of
        // forcing allocations into libc and potentially mixing allocator domains.
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
        void *records = ::mmap(nullptr, sizeof(LiveAllocation) * kEventCapacity,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void *index = ::mmap(nullptr, sizeof(LiveIndexEntry) * kLiveIndexCapacity,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (records == MAP_FAILED || index == MAP_FAILED) {
            if (records != MAP_FAILED) {
                ::munmap(records, sizeof(LiveAllocation) * kEventCapacity);
            }
            if (index != MAP_FAILED) {
                ::munmap(index, sizeof(LiveIndexEntry) * kLiveIndexCapacity);
            }
            error = "mmap for Linux allocation lifecycle tracking failed: " +
                    std::string(std::strerror(errno));
            return false;
        }
        live_storage = static_cast<LiveAllocation *>(records);
        live_index = static_cast<LiveIndexEntry *>(index);
        free_live = nullptr;
        for (std::size_t i = 0; i < kEventCapacity; ++i) {
            live_storage[i].next = free_live;
            free_live = &live_storage[i];
        }
        return true;
    }

    void releaseLifecycleStorage() noexcept
    {
        if (live_storage != nullptr) {
            ::munmap(live_storage, sizeof(LiveAllocation) * kEventCapacity);
            live_storage = nullptr;
        }
        if (live_index != nullptr) {
            ::munmap(live_index, sizeof(LiveIndexEntry) * kLiveIndexCapacity);
            live_index = nullptr;
        }
        free_live = nullptr;
    }

    bool prepareHooks(std::string &error)
    {
        if (!thread_state_key_created) {
            if (::pthread_key_create(&thread_state_key, &releaseThreadState) != 0) {
                error = "pthread_key_create for allocation thread state failed";
                return false;
            }
            thread_state_key_created = true;
        }
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

        const std::array specs{
            ElfImportHookSpec{"malloc", reinterpret_cast<void *>(&hookMalloc), true},
            ElfImportHookSpec{"calloc", reinterpret_cast<void *>(&hookCalloc), true},
            ElfImportHookSpec{"realloc", reinterpret_cast<void *>(&hookRealloc), true},
            ElfImportHookSpec{"free", reinterpret_cast<void *>(&hookFree), true},
            ElfImportHookSpec{"reallocarray", reinterpret_cast<void *>(&hookReallocArray), false},
            ElfImportHookSpec{"aligned_alloc", reinterpret_cast<void *>(&hookAlignedAlloc), false},
            ElfImportHookSpec{"posix_memalign", reinterpret_cast<void *>(&hookPosixMemalign), false},
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

    void updateHookCapabilities()
    {
        hook_capabilities.clear();
        for (const ElfImportHookCapability &capability : hooks.capabilities()) {
            hook_capabilities.push_back(
                {capability.name,
                 capability.available ? AllocationHookStatus::Active
                                      : AllocationHookStatus::Missing,
                 capability.detail});
        }
    }

    bool installHooks(std::string &error)
    {
        if (hooks.installed()) {
            return true;
        }
        Impl *expected = nullptr;
        if (!active_instance.compare_exchange_strong(expected, this,
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed) && expected != this) {
            error = "another native allocation sampler backend is already active";
            return false;
        }
        if (!hooks.install(error)) {
            expected = this;
            active_instance.compare_exchange_strong(expected, nullptr,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    bool waitFor(std::atomic<std::uint64_t> &counter, const char *description,
                 std::string &error) noexcept
    {
        for (int attempt = 0; attempt < 5000; ++attempt) {
            if (counter.load(std::memory_order_acquire) == 0) {
                return true;
            }
            timespec delay{0, 1000000};
            ::nanosleep(&delay, nullptr);
        }
        try {
            error = std::string("timed out waiting for ") + description + " to quiesce";
        }
        catch (...) {
        }
        return false;
    }

    FrameKey frameKey(cpptrace::frame_ptr address)
    {
        cpptrace::safe_object_frame object;
        cpptrace::get_safe_object_frame(address, &object);
        std::string_view path = object.object_path[0] != '\0'
                                    ? std::string_view(object.object_path)
                                    : std::string_view("unknown");
        FrameKey key;
        key.module = modules.intern(path);
        key.rva = static_cast<std::uint64_t>(object.address_relative_to_object_start);
        key.raw_address = static_cast<std::uint64_t>(object.raw_address);
        return key;
    }

    void acceptSample(const Sample &sample)
    {
        bool complete = tree.logBounded(sample.frames, sample.window, sample.weight,
                                        profile_nodes_remaining);
        auto [it, inserted] = thread_trees.try_emplace(sample.thread_id);
        ThreadCallTree &thread = it->second;
        if (inserted) {
            thread.thread_id = sample.thread_id;
            thread.thread_name = sample.thread_name;
        }
        complete = thread.tree.logBounded(sample.frames, sample.window,
                                          sample.weight, profile_nodes_remaining) &&
                   complete;
        if (!complete) {
            profile_nodes_exhausted.store(true, std::memory_order_relaxed);
        }
        sample_count.fetch_add(1, std::memory_order_relaxed);
        sampled_bytes.fetch_add(sample.weight, std::memory_order_relaxed);
    }

    bool buildSample(const cpptrace::frame_ptr *frames, std::uint16_t depth,
                     std::uint64_t tick_id, std::uint64_t thread_id,
                     std::uint64_t os_thread_id, std::int32_t window,
                     std::uint64_t weight, Sample &sample)
    {
        sample.tick_id = tick_id;
        const AllocationThreadSelection selection =
            thread_filter.resolve(thread_id, os_thread_id);
        if (!selection.selected) {
            filtered_samples.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        sample.thread_id = selection.profile_thread_id;
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

    bool tickAccepts(std::uint64_t tick_id) const noexcept
    {
        if (config.only_ticks_over_ms <= 0) {
            return true;
        }
        return tick_id < tick_decisions.size() &&
               tick_decisions[static_cast<std::size_t>(tick_id)] == 2;
    }

    void processEvent(const AllocationEvent &event)
    {
        if (event.thread_observation) {
            thread_filter.resolve(event.thread_id, event.os_thread_id);
            return;
        }
        Sample sample;
        if (!buildSample(event.frames, event.depth, event.tick_id, event.thread_id,
                         event.os_thread_id, event.window, event.weight_bytes,
                         sample)) {
            return;
        }
        if (config.only_ticks_over_ms <= 0) {
            acceptSample(sample);
        }
        else if (sample.tick_id < tick_decisions.size() &&
                 tick_decisions[static_cast<std::size_t>(sample.tick_id)] != 0) {
            if (tick_decisions[static_cast<std::size_t>(sample.tick_id)] == 2) {
                acceptSample(sample);
            }
        }
        else {
            if (sample.tick_id >= kMaxTickDecisions ||
                pending_samples == kMaxPendingSamples) {
                dropped_samples.fetch_add(1, std::memory_order_relaxed);
            }
            else {
                buckets[sample.tick_id].push_back(std::move(sample));
                ++pending_samples;
            }
        }
    }

    void finalizeLiveProfile()
    {
        const std::uint64_t stopped_ms = monotonicMs();
        std::uint64_t total_age = 0;
        std::uint64_t maximum_age = 0;
        for (std::size_t i = 0; i < kLiveIndexCapacity; ++i) {
            const LiveIndexEntry &entry = live_index[i];
            if (entry.pointer == nullptr ||
                entry.pointer == reinterpret_cast<void *>(static_cast<std::uintptr_t>(1)) ||
                entry.allocation == nullptr) {
                continue;
            }
            const LiveAllocation &allocation = *entry.allocation;
            if (!tickAccepts(allocation.tick_id)) {
                continue;
            }
            Sample sample;
            if (buildSample(allocation.frames, allocation.depth, allocation.tick_id,
                            allocation.thread_id, allocation.os_thread_id,
                            allocation.window, allocation.weight_bytes, sample)) {
                const std::uint64_t age = stopped_ms >= allocation.allocated_ms
                                              ? stopped_ms - allocation.allocated_ms
                                              : 0;
                total_age += age;
                maximum_age = (std::max)(maximum_age, age);
                acceptSample(sample);
            }
        }
        retained_age_ms_total.store(total_age, std::memory_order_relaxed);
        retained_age_ms_max.store(maximum_age, std::memory_order_relaxed);
    }

    void flushOrDrop(std::uint64_t tick_id, bool keep)
    {
        auto found = buckets.find(tick_id);
        if (found == buckets.end()) {
            return;
        }
        if (keep) {
            for (const Sample &sample : found->second) {
                acceptSample(sample);
            }
        }
        pending_samples -= found->second.size();
        buckets.erase(found);
    }

    void drainQueues()
    {
        TickEvent tick;
        while (ticks.dequeue(tick)) {
            const bool keep = config.only_ticks_over_ms <= 0 ||
                              tick.mspt_ms > static_cast<double>(config.only_ticks_over_ms);
            if (config.only_ticks_over_ms > 0 &&
                tick.tick_id < kMaxTickDecisions) {
                if (tick_decisions.size() <= tick.tick_id) {
                    tick_decisions.resize(static_cast<std::size_t>(tick.tick_id + 1), 0);
                }
                tick_decisions[static_cast<std::size_t>(tick.tick_id)] = keep ? 2 : 1;
            }
            flushOrDrop(tick.tick_id, keep);
        }
        AllocationEvent event;
        while (events.dequeue(event)) {
            processEvent(event);
        }
    }

    void aggregatorLoop()
    {
        TrackingSuppressionGuard suppress(*this);
        if (config.fail_aggregator_for_testing) {
            throw std::runtime_error("injected allocation aggregator failure");
        }
        if (config.aggregator_delay_ms_for_testing != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                config.aggregator_delay_ms_for_testing));
        }
        while (aggregator_running.load(std::memory_order_acquire)) {
            drainQueues();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        drainQueues();
        if (pending_samples != 0) {
            dropped_samples.fetch_add(pending_samples, std::memory_order_relaxed);
            pending_samples = 0;
        }
        buckets.clear();
    }

    void markAggregatorFailure(const char *message) noexcept
    {
        tracking.store(false, std::memory_order_release);
        aggregator_running.store(false, std::memory_order_release);
        std::snprintf(aggregator_failure.data(), aggregator_failure.size(), "%s",
                      message != nullptr ? message : "unknown allocation aggregator failure");
        aggregator_failed.store(true, std::memory_order_release);
    }

    void resetSession()
    {
        TickEvent tick;
        while (ticks.dequeue(tick)) {
        }
        tree = CallTree{};
        thread_trees.clear();
        thread_filter.clear();
        modules = ModuleTable{kMaxAllocationModules};
        window_ticks.clear();
        buckets.clear();
        pending_samples = 0;
        profile_nodes_remaining = kMaxProfileNodes;
        profile_nodes_exhausted.store(false, std::memory_order_relaxed);
        tick_decisions.clear();
        current_tick.store(0, std::memory_order_relaxed);
        hook_calls.store(0, std::memory_order_relaxed);
        successful_allocation_calls.store(0, std::memory_order_relaxed);
        sample_count.store(0, std::memory_order_relaxed);
        sampling_points.store(0, std::memory_order_relaxed);
        sampled_bytes.store(0, std::memory_order_relaxed);
        filtered_samples.store(0, std::memory_order_relaxed);
        observed_bytes.store(0, std::memory_order_relaxed);
        dropped_samples.store(0, std::memory_order_relaxed);
        dropped_events.store(0, std::memory_order_relaxed);
        dropped_tick_events.store(0, std::memory_order_relaxed);
        enqueued_samples.store(0, std::memory_order_relaxed);
        tracking_calls.store(0, std::memory_order_relaxed);
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
        retained_age_ms_total.store(0, std::memory_order_relaxed);
        retained_age_ms_max.store(0, std::memory_order_relaxed);
        aggregator_failure.fill('\0');
        aggregator_failed.store(false, std::memory_order_release);
    }

    bool startSession(const AllocationSamplerConfig &new_config, std::string &error)
    {
        std::scoped_lock lock(lifecycle_mutex);
        error.clear();
        if (running.load(std::memory_order_acquire)) {
            error = "allocation profiler is already running";
            return false;
        }
        if (aggregator_thread.joinable()) {
            error = "the previous allocation session has not finished cleanup";
            return false;
        }
        if (new_config.session_seed == 0 || new_config.interval_bytes <= 0) {
            error = "invalid Linux allocation sampler configuration";
            return false;
        }
        resetSession();
        config = new_config;
        if (!thread_filter.configure(new_config.all_threads,
                                     new_config.regex_threads,
                                     new_config.thread_patterns, error)) {
            return false;
        }
        interval_bytes.store(static_cast<std::uint64_t>(new_config.interval_bytes),
                             std::memory_order_relaxed);
        started_ms.store(monotonicMs(), std::memory_order_relaxed);
        last_module_rescan_ms = started_ms.load(std::memory_order_relaxed);
        const std::uint64_t next_generation =
            generation.fetch_add(1, std::memory_order_relaxed) + 1;
        sampling_seed.store(next_generation ^ monotonicMs() ^ new_config.session_seed,
                            std::memory_order_relaxed);

        if (!prepareHooks(error) || !events.allocate(error) ||
            !allocateLifecycleStorage(error) || !installHooks(error)) {
            events.release();
            releaseLifecycleStorage();
            return false;
        }

        aggregator_running.store(true, std::memory_order_release);
        running.store(true, std::memory_order_release);
        tracking.store(true, std::memory_order_release);
        try {
            TrackingSuppressionGuard suppress(*this);
            aggregator_thread = std::thread([this] {
                try {
                    aggregatorLoop();
                }
                catch (const std::exception &exception) {
                    markAggregatorFailure(exception.what());
                }
                catch (...) {
                    markAggregatorFailure("allocation aggregator failed with an unknown exception");
                }
            });
        }
        catch (...) {
            tracking.store(false, std::memory_order_release);
            running.store(false, std::memory_order_release);
            aggregator_running.store(false, std::memory_order_release);
            std::string quiescence_error;
            if (!waitFor(tracking_calls, "tracked Linux allocation hooks",
                         quiescence_error)) {
                error = std::move(quiescence_error);
                return false;
            }
            events.release();
            releaseLifecycleStorage();
            error = "could not create the allocation aggregator thread";
            return false;
        }
        return true;
    }

    bool stopSession(std::string &error)
    {
        std::scoped_lock lock(lifecycle_mutex);
        error.clear();
        if (!running.load(std::memory_order_acquire) && !aggregator_thread.joinable()) {
            return true;
        }
        tracking.store(false, std::memory_order_release);
        running.store(false, std::memory_order_release);
        if (!waitFor(tracking_calls, "tracked Linux allocation hooks", error)) {
            return false;
        }
        aggregator_running.store(false, std::memory_order_release);
        if (aggregator_thread.joinable()) {
            aggregator_thread.join();
        }
        if (config.live_only && !aggregator_failed.load(std::memory_order_acquire)) {
            finalizeLiveProfile();
        }
        events.release();
        releaseLifecycleStorage();
        if (aggregator_failed.load(std::memory_order_acquire)) {
            error = "allocation aggregator failed: " + std::string(aggregator_failure.data());
            return false;
        }
        if (config.live_only && lifecycle_dropped.load(std::memory_order_relaxed) != 0) {
            error = "allocation lifecycle tracking capacity was exhausted; retained profile discarded";
            return false;
        }
        return true;
    }

    bool shutdownBackend(std::string &error)
    {
        std::scoped_lock lock(lifecycle_mutex);
        error.clear();
        tracking.store(false, std::memory_order_release);
        running.store(false, std::memory_order_release);
        if (!waitFor(tracking_calls, "tracked Linux allocation hooks", error)) {
            return false;
        }
        aggregator_running.store(false, std::memory_order_release);
        if (aggregator_thread.joinable()) {
            aggregator_thread.join();
        }
        events.release();
        releaseLifecycleStorage();

        if (hooks.installed() && !hooks.uninstall(error)) {
            return false;
        }
        if (!waitFor(active_hook_calls, "Linux allocation hook thunks", error)) {
            return false;
        }
        Impl *expected = this;
        active_instance.compare_exchange_strong(expected, nullptr,
                                                std::memory_order_release,
                                                std::memory_order_relaxed);
        releaseThreadStateRegistry();
        return true;
    }

    void releaseThreadStateRegistry() noexcept
    {
        if (thread_state_key_created) {
            ::pthread_key_delete(thread_state_key);
            thread_state_key_created = false;
        }
        for (ThreadSamplingState &state : thread_states) {
            state.owner_tid.store(0, std::memory_order_relaxed);
            state.registry_state.store(0, std::memory_order_relaxed);
        }
    }

    void tick(double mspt_ms)
    {
        if (!running.load(std::memory_order_acquire) ||
            aggregator_failed.load(std::memory_order_acquire)) {
            return;
        }
        TrackingSuppressionGuard suppress(*this);
        const std::uint64_t finished = current_tick.fetch_add(1, std::memory_order_relaxed);
        if (config.only_ticks_over_ms > 0 &&
            !ticks.enqueue(TickEvent{finished, mspt_ms})) {
            dropped_tick_events.fetch_add(1, std::memory_order_relaxed);
        }
        const std::uint64_t now = monotonicMs();
        if (now >= last_module_rescan_ms + 5000) {
            std::unique_lock lock(lifecycle_mutex, std::try_to_lock);
            if (lock.owns_lock()) {
                std::string ignored;
                if (hooks.rescan(ignored)) {
                    updateHookCapabilities();
                }
                last_module_rescan_ms = now;
            }
        }
        const std::uint64_t started = started_ms.load(std::memory_order_relaxed);
        const std::int32_t window = static_cast<std::int32_t>((std::min)(
            static_cast<std::uint64_t>(kMaxProfileWindows - 1),
            started != 0 && now >= started ? (now - started) / 1000 : 0));
        WindowTickStats &stats = window_ticks[window];
        ++stats.ticks;
        stats.mspt_sum += mspt_ms;
        stats.mspt_max = (std::max)(stats.mspt_max, mspt_ms);
    }
};

std::atomic<AllocationSampler::Impl *> AllocationSampler::Impl::active_instance{nullptr};
std::atomic<std::uint64_t> AllocationSampler::Impl::active_hook_calls{0};

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
        std::fprintf(stderr, "[spark] Linux allocation sampler shutdown failed: %s\n",
                     error.c_str());
    }
    std::abort();
}

bool AllocationSampler::start(const AllocationSamplerConfig &config, std::string &error)
{
    return impl_->startSession(config, error);
}

bool AllocationSampler::stop(std::string &error) { return impl_->stopSession(error); }
bool AllocationSampler::shutdown(std::string &error) { return impl_->shutdownBackend(error); }
void AllocationSampler::onTick(double mspt_ms) { impl_->tick(mspt_ms); }
const CallTree &AllocationSampler::tree() const { return impl_->tree; }
const std::map<std::uint64_t, ThreadCallTree> &AllocationSampler::threadTrees() const
{
    return impl_->thread_trees;
}
const ModuleTable &AllocationSampler::modules() const { return impl_->modules; }
const std::map<std::int32_t, WindowTickStats> &AllocationSampler::windowTicks() const
{
    return impl_->window_ticks;
}
std::uint64_t AllocationSampler::numberOfTicks() const
{
    return impl_->current_tick.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::hookCalls() const
{
    return impl_->hook_calls.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::successfulAllocationCalls() const
{
    return impl_->successful_allocation_calls.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::sampleCount() const
{
    return impl_->sample_count.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::samplingPoints() const
{
    return impl_->sampling_points.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::sampledBytes() const
{
    return impl_->sampled_bytes.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::filteredSamples() const
{
    return impl_->filtered_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::threadNameFailures() const
{
    return impl_->thread_filter.nameFailures();
}
std::uint64_t AllocationSampler::threadIdentityCacheDrops() const
{
    return impl_->thread_filter.cacheDrops();
}
std::uint64_t AllocationSampler::observedBytes() const
{
    return impl_->observed_bytes.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::droppedSamples() const
{
    return impl_->dropped_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::droppedEvents() const
{
    return impl_->dropped_events.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::droppedTickEvents() const
{
    return impl_->dropped_tick_events.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::tickEventCapacity() const
{
    return kTickEventCapacity;
}
std::uint64_t AllocationSampler::enqueuedSamples() const
{
    return impl_->enqueued_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::eventQueueHighWaterMark() const
{
    return impl_->events.high_water.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::eventQueueCapacity() const
{
    return kEventCapacity;
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
std::uint64_t AllocationSampler::liveIndexCapacity() const
{
    return kEventCapacity;
}
std::uint64_t AllocationSampler::sampledThreadCount() const
{
    return impl_->thread_trees.size();
}
std::uint64_t AllocationSampler::threadRootCapacity() const
{
    return kMaxSampledThreads + 1;
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
    return impl_->hooks.hookedModuleCount();
}
std::uint64_t AllocationSampler::skippedModuleCount() const
{
    return impl_->hooks.skippedModuleCount();
}
std::uint64_t AllocationSampler::failedModuleCount() const
{
    return impl_->hooks.failedModuleCount();
}
std::uint64_t AllocationSampler::moduleRegistryCount() const
{
    return impl_->modules.size();
}
std::uint64_t AllocationSampler::moduleRegistryCapacity() const
{
    return kMaxAllocationModules;
}
std::uint64_t AllocationSampler::profileNodeCapacity() const
{
    return kMaxProfileNodes;
}
bool AllocationSampler::dataIncomplete() const
{
    return impl_->dropped_samples.load(std::memory_order_relaxed) != 0 ||
           impl_->lifecycle_dropped.load(std::memory_order_relaxed) != 0 ||
           impl_->dropped_tick_events.load(std::memory_order_relaxed) != 0 ||
           impl_->thread_state_drops.load(std::memory_order_relaxed) != 0 ||
           impl_->thread_filter.cacheDrops() != 0 ||
           impl_->profile_nodes_exhausted.load(std::memory_order_relaxed) ||
           impl_->hooks.failedModuleCount() != 0;
}
std::uint64_t AllocationSampler::averageLifetimeMs() const
{
    const std::uint64_t count = impl_->freed_samples.load(std::memory_order_relaxed);
    return count == 0 ? 0 :
                        impl_->lifetime_ms_total.load(std::memory_order_relaxed) / count;
}
std::uint64_t AllocationSampler::maximumLifetimeMs() const
{
    return impl_->lifetime_ms_max.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::lifecycleDropped() const
{
    return impl_->lifecycle_dropped.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::retainedAverageAgeMs() const
{
    const std::uint64_t count = impl_->sample_count.load(std::memory_order_relaxed);
    return count == 0 ? 0 :
                        impl_->retained_age_ms_total.load(std::memory_order_relaxed) / count;
}
std::uint64_t AllocationSampler::retainedMaximumAgeMs() const
{
    return impl_->retained_age_ms_max.load(std::memory_order_relaxed);
}
bool AllocationSampler::running() const
{
    return impl_->running.load(std::memory_order_acquire);
}
bool AllocationSampler::hooksInstalled() const { return impl_->hooks.installed(); }
bool AllocationSampler::failure(std::string &error) const
{
    if (!impl_->aggregator_failed.load(std::memory_order_acquire)) {
        error.clear();
        return false;
    }
    error = impl_->aggregator_failure.data();
    return true;
}
const std::vector<AllocationHookCapability> &AllocationSampler::hookCapabilities() const
{
    return impl_->hook_capabilities;
}
std::size_t AllocationSampler::hookTargetCount() const { return impl_->hooks.targetCount(); }

}  // namespace spark
