#include "native/alloc/allocation_sampler.h"

#if !defined(_WIN32)
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
#include <string>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <funchook.h>
#include "native/alloc/allocation_thread_filter.h"
#include "native/alloc/bounded_event_queue.h"
#include "native/alloc/byte_sampler.h"
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
constexpr std::size_t kMaxThreadStates = 2048;
constexpr std::size_t kMaxAllocationModules = 512;
constexpr std::size_t kMaxModuleCacheEntries = 1024;
constexpr std::size_t kMaxProfileNodes = 131072;
constexpr std::size_t kMaxPendingSamples = 32768;
constexpr std::size_t kMaxTickDecisions = 100000;
constexpr std::size_t kTickEventCapacity = 4096;
constexpr std::int32_t kMaxProfileWindows = 86400;
constexpr std::size_t kMaxPatchThreads = 2048;
constexpr std::size_t kHookPatchSize = 5;  // funchook 1.1.3 x86/x64 entry jump
constexpr unsigned long kFramesToSkip = 2;

struct CodeRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

struct PreparedTarget {
    void *address = nullptr;
    std::array<std::byte, kHookPatchSize> original{};
    std::string export_name;
};

std::uint64_t monotonicMs() noexcept
{
    return static_cast<std::uint64_t>(::GetTickCount64());
}

std::uint64_t saturatingMultiply(std::uint64_t a, std::uint64_t b) noexcept
{
    const std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
    if (a == 0 || b == 0) {
        return 0;
    }
    return a > max / b ? max : a * b;
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

std::string moduleBasename(const std::string &path)
{
    const std::size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool equalsIgnoreCase(const std::string &a, const char *b)
{
    return ::_stricmp(a.c_str(), b) == 0;
}

bool isLeadingAllocatorRuntime(const std::string &path)
{
    const std::string name = moduleBasename(path);
    return equalsIgnoreCase(name, "spark.dll") || equalsIgnoreCase(name, "ucrtbase.dll") ||
           equalsIgnoreCase(name, "vcruntime140.dll") || equalsIgnoreCase(name, "vcruntime140_1.dll") ||
           equalsIgnoreCase(name, "msvcp140.dll");
}

class SuspendedProcessThreads {
public:
    SuspendedProcessThreads()
    {
        threads_.reserve(kMaxPatchThreads);
    }

    SuspendedProcessThreads(const SuspendedProcessThreads &) = delete;
    SuspendedProcessThreads &operator=(const SuspendedProcessThreads &) = delete;

    ~SuspendedProcessThreads()
    {
        std::string ignored;
        resume(ignored);
        closeHandles();
    }

    // Re-snapshot after each pass while already-known threads are suspended. This
    // closes the common race where a thread appears between the first snapshot and
    // the code patch. No vector allocation is permitted after the first thread has
    // been suspended: capacity is reserved up front and excessive thread counts
    // fail safely.
    bool suspendStable(std::string &error)
    {
        DWORD failure = ERROR_SUCCESS;
        const char *operation = nullptr;

        for (int pass = 0; pass < 4; ++pass) {
            bool added = false;
            HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE) {
                failure = ::GetLastError();
                operation = "CreateToolhelp32Snapshot";
                break;
            }

            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (::Thread32First(snapshot, &entry) == FALSE) {
                failure = ::GetLastError();
                operation = "Thread32First";
                ::CloseHandle(snapshot);
                break;
            }

            const DWORD process_id = ::GetCurrentProcessId();
            const DWORD current_thread_id = ::GetCurrentThreadId();
            do {
                if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == current_thread_id ||
                    contains(entry.th32ThreadID)) {
                    continue;
                }
                if (threads_.size() == threads_.capacity()) {
                    failure = ERROR_NOT_ENOUGH_MEMORY;
                    operation = "thread suspension capacity";
                    break;
                }

                HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                                 THREAD_QUERY_LIMITED_INFORMATION,
                                              FALSE, entry.th32ThreadID);
                if (thread == nullptr) {
                    const DWORD code = ::GetLastError();
                    if (code == ERROR_INVALID_PARAMETER) {
                        continue;  // thread exited after the snapshot
                    }
                    failure = code;
                    operation = "OpenThread";
                    break;
                }

                const DWORD previous_count = ::SuspendThread(thread);
                if (previous_count == static_cast<DWORD>(-1)) {
                    DWORD exit_code = STILL_ACTIVE;
                    const DWORD code = ::GetLastError();
                    if (::GetExitCodeThread(thread, &exit_code) != FALSE && exit_code != STILL_ACTIVE) {
                        ::CloseHandle(thread);
                        continue;
                    }
                    ::CloseHandle(thread);
                    failure = code;
                    operation = "SuspendThread";
                    break;
                }

                threads_.push_back({thread, entry.th32ThreadID, previous_count, true});
                added = true;
            } while (failure == ERROR_SUCCESS && ::Thread32Next(snapshot, &entry) != FALSE);

            if (failure == ERROR_SUCCESS) {
                const DWORD iteration_error = ::GetLastError();
                if (iteration_error != ERROR_NO_MORE_FILES) {
                    failure = iteration_error;
                    operation = "Thread32Next";
                }
            }

            ::CloseHandle(snapshot);
            if (failure != ERROR_SUCCESS) {
                break;
            }
            if (!added) {
                return true;
            }
        }

        if (failure == ERROR_SUCCESS) {
            failure = ERROR_BUSY;
            operation = "thread set did not stabilize";
        }

        std::string resume_error;
        resume(resume_error);
        error = std::string(operation != nullptr ? operation : "thread suspension") +
                " failed: " + std::to_string(failure);
        if (!resume_error.empty()) {
            error += "; " + resume_error;
        }
        return false;
    }

    bool resume(std::string &error) noexcept
    {
        DWORD first_failure = ERROR_SUCCESS;
        DWORD first_thread = 0;
        for (auto it = threads_.rbegin(); it != threads_.rend(); ++it) {
            if (!it->suspended) {
                continue;
            }
            bool resumed = false;
            DWORD last_error = ERROR_SUCCESS;
            for (int attempt = 0; attempt < 100; ++attempt) {
                if (::ResumeThread(it->handle) != static_cast<DWORD>(-1)) {
                    resumed = true;
                    break;
                }
                last_error = ::GetLastError();
                DWORD exit_code = STILL_ACTIVE;
                if (::GetExitCodeThread(it->handle, &exit_code) != FALSE && exit_code != STILL_ACTIVE) {
                    resumed = true;  // exited threads no longer need resuming
                    break;
                }
                ::Sleep(1);
            }
            if (!resumed && first_failure == ERROR_SUCCESS) {
                first_failure = last_error != ERROR_SUCCESS ? last_error : ::GetLastError();
                first_thread = it->thread_id;
            }
            else if (resumed) {
                it->suspended = false;
            }
        }

        if (first_failure != ERROR_SUCCESS) {
            try {
                error = "ResumeThread failed for thread " + std::to_string(first_thread) +
                        ": " + std::to_string(first_failure);
            }
            catch (...) {
                // noexcept cleanup path: retaining an empty message is preferable
                // to terminating while attempting to resume process threads.
            }
            // Returning would leave BDS running with a thread suspended by us,
            // which can deadlock unrelated subsystems and makes subsequent hook
            // cleanup impossible to prove. This is an unrecoverable process
            // consistency failure, so terminate without invoking CRT cleanup.
            ::TerminateProcess(::GetCurrentProcess(), first_failure);
            std::abort();  // defensive fallback if TerminateProcess unexpectedly returns
        }
        return true;
    }

    bool anyInstructionPointerInRanges(const std::vector<CodeRange> &ranges, bool &found,
                                       DWORD &failure, DWORD &failed_thread) const noexcept
    {
        found = false;
        failure = ERROR_SUCCESS;
        failed_thread = 0;
        for (const ThreadRecord &thread : threads_) {
            if (!thread.suspended) {
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (::GetThreadContext(thread.handle, &context) == FALSE) {
                DWORD exit_code = STILL_ACTIVE;
                if (::GetExitCodeThread(thread.handle, &exit_code) != FALSE && exit_code != STILL_ACTIVE) {
                    continue;
                }
                failure = ::GetLastError();
                failed_thread = thread.thread_id;
                return false;
            }
#if defined(_M_X64)
            const std::uintptr_t instruction = static_cast<std::uintptr_t>(context.Rip);
#elif defined(_M_ARM64)
            const std::uintptr_t instruction = static_cast<std::uintptr_t>(context.Pc);
#else
#error "Windows allocation profiler requires a supported 64-bit CONTEXT"
#endif
            for (const CodeRange &range : ranges) {
                if (instruction >= range.begin && instruction < range.end) {
                    found = true;
                    return true;
                }
            }
        }
        return true;
    }

private:
    struct ThreadRecord {
        HANDLE handle = nullptr;
        DWORD thread_id = 0;
        DWORD previous_suspend_count = 0;
        bool suspended = false;
    };

    bool contains(DWORD thread_id) const noexcept
    {
        return std::any_of(threads_.begin(), threads_.end(), [thread_id](const ThreadRecord &record) {
            return record.thread_id == thread_id;
        });
    }

    void closeHandles() noexcept
    {
        for (ThreadRecord &thread : threads_) {
            if (thread.handle != nullptr) {
                ::CloseHandle(thread.handle);
                thread.handle = nullptr;
            }
        }
        threads_.clear();
    }

    std::vector<ThreadRecord> threads_;
};

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
        void *frames[kStackDepth]{};
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
        void *frames[kStackDepth]{};
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

    static std::atomic<Impl *> active_instance;
    static std::array<HookCounter, 64> active_hook_calls;

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

    class HookCallGuard {
    public:
        HookCallGuard() noexcept
        {
            const std::uintptr_t thread_key =
                reinterpret_cast<std::uintptr_t>(::NtCurrentTeb()) >> 12;
            counter_ = &active_hook_calls[
                static_cast<std::size_t>(thread_key) %
                active_hook_calls.size()];
            counter_->value.fetch_add(1, std::memory_order_acq_rel);
        }

        ~HookCallGuard()
        {
            counter_->value.fetch_sub(1, std::memory_order_release);
        }

        HookCallGuard(const HookCallGuard &) = delete;
        HookCallGuard &operator=(const HookCallGuard &) = delete;

    private:
        HookCounter *counter_ = nullptr;
    };

    class TrackingCallGuard {
    public:
        explicit TrackingCallGuard(Impl &impl) noexcept : impl_(impl)
        {
            if (!impl_.tracking.load(std::memory_order_acquire)) {
                return;
            }
            impl_.tracking_hook_calls.fetch_add(1, std::memory_order_acq_rel);
            if (impl_.tracking.load(std::memory_order_acquire)) {
                active_ = true;
                return;
            }
            impl_.tracking_hook_calls.fetch_sub(1, std::memory_order_release);
        }

        ~TrackingCallGuard()
        {
            if (active_) {
                impl_.tracking_hook_calls.fetch_sub(1, std::memory_order_release);
            }
        }

        explicit operator bool() const noexcept
        {
            return active_;
        }

    private:
        Impl &impl_;
        bool active_ = false;
    };

    class RecursionGuard {
    public:
        explicit RecursionGuard(Impl &impl) noexcept : state_(impl.currentThreadState())
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

        bool owner() const noexcept
        {
            return owner_;
        }

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

    funchook_t *hooks = nullptr;
    bool hooks_prepared = false;
    bool hook_state_unknown = false;
    DWORD tls_index = TLS_OUT_OF_INDEXES;
    std::atomic<bool> hooks_installed{false};
    std::atomic<bool> tracking{false};
    std::atomic<bool> running{false};
    std::atomic<bool> aggregator_running{false};
    std::atomic<bool> aggregator_failed{false};
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
    std::vector<PreparedTarget> prepared_targets;
    std::vector<CodeRange> protected_code_ranges;
    std::vector<AllocationHookCapability> hook_capabilities;
    AllocationSamplerConfig config{};
    std::atomic<std::uint64_t> current_tick{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> interval_bytes{kDefaultAllocationIntervalBytes};
    std::atomic<std::uint64_t> sampling_seed{0};
    std::atomic<std::uint64_t> started_tick_ms{0};
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
    std::atomic<std::uint64_t> ready_event_count{0};
    std::atomic<std::uint64_t> ready_event_high_water{0};
    std::atomic<std::uint64_t> tracking_hook_calls{0};
    std::atomic<std::uint64_t> next_allocation_id{1};
    std::atomic<std::uint64_t> next_session_thread_id{1};
    std::atomic<std::uint64_t> registered_threads{0};
    std::atomic<std::uint64_t> overflow_threads{0};
    std::atomic<std::uint64_t> thread_state_drops{0};
    std::atomic<bool> profile_nodes_exhausted{false};
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
    SLIST_HEADER free_events{};
    SLIST_HEADER ready_events{};
    AllocationEvent *event_storage = nullptr;
    SLIST_HEADER free_live_allocations{};
    LiveAllocation *live_storage = nullptr;
    LiveIndexEntry *live_index = nullptr;
    std::array<SRWLOCK, kLiveIndexShards> live_index_locks{};
    std::array<ThreadSamplingState, kMaxThreadStates> thread_states{};

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
    std::unordered_map<std::uintptr_t, ModuleId> module_cache;

    RecoverySink *recovery_sink_ = nullptr;
    std::unordered_set<std::uint64_t> journaled_threads_;

    ~Impl() = default;

    static Impl *active() noexcept
    {
        return active_instance.load(std::memory_order_acquire);
    }

    static Impl *activeOrAbort() noexcept
    {
        Impl *self = active();
        if (self == nullptr) {
            std::abort();
        }
        if (self->tracking.load(std::memory_order_relaxed)) {
            self->hook_calls.fetch_add(1, std::memory_order_relaxed);
        }
        return self;
    }

    static void *__cdecl hookMalloc(std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleMalloc(self->real_malloc, size);
    }

    static void *__cdecl hookCalloc(std::size_t count, std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleCalloc(self->real_calloc, count, size);
    }

    static void *__cdecl hookRealloc(void *pointer, std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleRealloc(self->real_realloc, pointer, size);
    }

    static void *__cdecl hookRecalloc(void *pointer, std::size_t count, std::size_t size) noexcept
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

    static void *__cdecl hookAlignedMalloc(std::size_t size, std::size_t alignment) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedMalloc(self->real_aligned_malloc, size, alignment);
    }

    static void *__cdecl hookAlignedRealloc(void *pointer, std::size_t size, std::size_t alignment) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedRealloc(self->real_aligned_realloc, pointer, size, alignment);
    }

    static void *__cdecl hookAlignedRecalloc(void *pointer, std::size_t count, std::size_t size,
                                             std::size_t alignment) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedRecalloc(self->real_aligned_recalloc, pointer, count, size, alignment);
    }

    static void *__cdecl hookAlignedOffsetMalloc(std::size_t size, std::size_t alignment,
                                                 std::size_t offset) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedOffsetMalloc(self->real_aligned_offset_malloc, size, alignment, offset);
    }

    static void *__cdecl hookAlignedOffsetRealloc(void *pointer, std::size_t size, std::size_t alignment,
                                                  std::size_t offset) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedOffsetRealloc(self->real_aligned_offset_realloc, pointer, size, alignment,
                                                offset);
    }

    static void *__cdecl hookAlignedOffsetRecalloc(void *pointer, std::size_t count, std::size_t size,
                                                   std::size_t alignment, std::size_t offset) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedOffsetRecalloc(self->real_aligned_offset_recalloc, pointer, count, size,
                                                 alignment, offset);
    }

    static void __cdecl hookAlignedFree(void *pointer) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        self->handleFree(self->real_aligned_free, pointer);
    }

    static void *__cdecl hookMallocBase(std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleMalloc(self->real_malloc_base, size);
    }

    static void *__cdecl hookCallocBase(std::size_t count, std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleCalloc(self->real_calloc_base, count, size);
    }

    static void *__cdecl hookReallocBase(void *pointer, std::size_t size) noexcept
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

    static void *WINAPI hookHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleHeapAlloc(self->real_heap_alloc, heap, flags, size);
    }

    static void *WINAPI hookHeapReAlloc(HANDLE heap, DWORD flags, void *pointer, SIZE_T size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleHeapReAlloc(self->real_heap_realloc, heap, flags, pointer, size);
    }

    static BOOL WINAPI hookHeapFree(HANDLE heap, DWORD flags, void *pointer) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleHeapFree(self->real_heap_free, heap, flags, pointer);
    }

    ThreadSamplingState *currentThreadState() noexcept
    {
        if (tls_index == TLS_OUT_OF_INDEXES) {
            return nullptr;
        }

        PTEB teb = ::NtCurrentTeb();
        // Reserved1[11] is the documented SDK view of the native TEB
        // ThreadLocalStoragePointer slot. It is null while LdrpAllocateTls is
        // building a new thread's static TLS vector; hooks must stay pass-through
        // until that loader operation finishes.
        if (teb == nullptr || teb->Reserved1[11] == nullptr) {
            return nullptr;
        }

        void *value = ::TlsGetValue(tls_index);
        if (value == reinterpret_cast<void *>(static_cast<std::uintptr_t>(1))) {
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
            if (candidate.registry_state.compare_exchange_strong(
                    expected, 1, std::memory_order_acq_rel)) {
                claimed = &candidate;
                break;
            }
            expected = 2;
            if (candidate.registry_state.compare_exchange_strong(
                    expected, 3, std::memory_order_acq_rel)) {
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
        if (::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                              ::GetCurrentProcess(), &thread_handle, SYNCHRONIZE,
                              FALSE, 0) != FALSE) {
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
        auto *self = const_cast<Impl *>(this);
        ThreadSamplingState *state = self->currentThreadState();
        return state != nullptr && !state->tracking_suppressed;
    }

    static std::uint64_t liveIndexHash(void *pointer) noexcept
    {
        const auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer) >> 4);
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
        std::uint64_t previous_max = lifetime_ms_max.load(std::memory_order_relaxed);
        while (previous_max < lifetime &&
               !lifetime_ms_max.compare_exchange_weak(previous_max, lifetime,
                                                      std::memory_order_relaxed)) {
        }
        ::InterlockedPushEntrySList(&free_live_allocations, &allocation->entry);
    }

    LiveAllocation *detachAllocation(void *pointer) noexcept
    {
        if (pointer == nullptr || live_index == nullptr) {
            return nullptr;
        }
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t shard = liveIndexShard(hash);
        ::AcquireSRWLockExclusive(&live_index_locks[shard]);
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
        ::ReleaseSRWLockExclusive(&live_index_locks[shard]);
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
            ::InterlockedPushEntrySList(&free_live_allocations,
                                        &allocation->entry);
        }
    }

    void handleFree(FreeFn function, void *pointer) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            function(pointer);
            return;
        }
        TrackingCallGuard tracking_call(*this);
        RecursionGuard recursion(*this);
        if (!tracking_call || !recursion.owner()) {
            function(pointer);
            return;
        }
        LiveAllocation *allocation = detachAllocation(pointer);
        function(pointer);
        if (allocation != nullptr) {
            retireAllocation(allocation, monotonicMs());
        }
    }

    BOOL handleHeapFree(HeapFreeFn function, HANDLE heap, DWORD flags, void *pointer) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(heap, flags, pointer);
        }
        TrackingCallGuard tracking_call(*this);
        RecursionGuard recursion(*this);
        if (!tracking_call || !recursion.owner()) {
            return function(heap, flags, pointer);
        }
        LiveAllocation *allocation = detachAllocation(pointer);
        const BOOL result = function(heap, flags, pointer);
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

    void *handleMalloc(MallocFn function, std::size_t requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(requested_size);
        }
        void *pointer = function(requested_size);
        if (pointer != nullptr) {
            recordAllocation(pointer, static_cast<std::uint64_t>(requested_size));
        }
        return pointer;
    }

    void *handleCalloc(CallocFn function, std::size_t count, std::size_t requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(count, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(count, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(count, requested_size);
        }
        void *pointer = function(count, requested_size);
        if (pointer != nullptr) {
            std::uint64_t bytes = 0;
            if (checkedMultiply(count, requested_size, bytes)) {
                recordAllocation(pointer, bytes);
            }
        }
        return pointer;
    }

    void *handleRealloc(ReallocFn function, void *pointer, std::size_t requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, requested_size);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, requested_size);
        const bool replaced =
            new_pointer != nullptr || (pointer != nullptr && requested_size == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && requested_size != 0) {
            // Allocation mode measures successful allocation requests. This avoids
            // an extra _msize heap query on every sample-path allocation and matches
            // the profiler's pressure-oriented semantics better than usable size.
            recordAllocation(new_pointer, static_cast<std::uint64_t>(requested_size));
        }
        return new_pointer;
    }

    void *handleRecalloc(RecallocFn function, void *pointer, std::size_t count,
                         std::size_t requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, count, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, count, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, count, requested_size);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, requested_size, bytes);
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, count, requested_size);
        const bool replaced =
            new_pointer != nullptr ||
            (pointer != nullptr && valid_size && bytes == 0);
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

    void *handleAlignedMalloc(AlignedMallocFn function, std::size_t size, std::size_t alignment) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(size, alignment);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(size, alignment);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(size, alignment);
        }
        void *pointer = function(size, alignment);
        if (pointer != nullptr) {
            recordAllocation(pointer, static_cast<std::uint64_t>(size));
        }
        return pointer;
    }

    void *handleAlignedRealloc(AlignedReallocFn function, void *pointer, std::size_t size,
                               std::size_t alignment) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, size, alignment);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, size, alignment);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, size, alignment);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, size, alignment);
        const bool replaced =
            new_pointer != nullptr || (pointer != nullptr && size == 0);
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

    void *handleAlignedRecalloc(AlignedRecallocFn function, void *pointer, std::size_t count,
                                std::size_t size, std::size_t alignment) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, count, size, alignment);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, count, size, alignment);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, count, size, alignment);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, size, bytes);
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, count, size, alignment);
        const bool replaced =
            new_pointer != nullptr ||
            (pointer != nullptr && valid_size && bytes == 0);
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

    void *handleAlignedOffsetMalloc(AlignedOffsetMallocFn function, std::size_t size,
                                    std::size_t alignment, std::size_t offset) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(size, alignment, offset);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(size, alignment, offset);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(size, alignment, offset);
        }
        void *pointer = function(size, alignment, offset);
        if (pointer != nullptr) {
            recordAllocation(pointer, static_cast<std::uint64_t>(size));
        }
        return pointer;
    }

    void *handleAlignedOffsetRealloc(AlignedOffsetReallocFn function, void *pointer, std::size_t size,
                                     std::size_t alignment, std::size_t offset) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, size, alignment, offset);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, size, alignment, offset);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, size, alignment, offset);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, size, alignment, offset);
        const bool replaced =
            new_pointer != nullptr || (pointer != nullptr && size == 0);
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
                                      std::size_t size, std::size_t alignment, std::size_t offset) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, count, size, alignment, offset);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, count, size, alignment, offset);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, count, size, alignment, offset);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, size, bytes);
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, count, size, alignment, offset);
        const bool replaced =
            new_pointer != nullptr ||
            (pointer != nullptr && valid_size && bytes == 0);
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


    void *handleHeapAlloc(HeapAllocFn function, HANDLE heap, DWORD flags, SIZE_T requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(heap, flags, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(heap, flags, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(heap, flags, requested_size);
        }
        void *pointer = function(heap, flags, requested_size);
        if (pointer != nullptr) {
            recordAllocation(pointer, static_cast<std::uint64_t>(requested_size));
        }
        return pointer;
    }

    void *handleHeapReAlloc(HeapReAllocFn function, HANDLE heap, DWORD flags, void *pointer,
                            SIZE_T requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(heap, flags, pointer, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(heap, flags, pointer, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(heap, flags, pointer, requested_size);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(heap, flags, pointer, requested_size);
        if (new_pointer != nullptr) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && requested_size != 0) {
            recordAllocation(new_pointer, static_cast<std::uint64_t>(requested_size));
        }
        return new_pointer;
    }

    bool insertLiveAllocation(LiveAllocation *allocation) noexcept
    {
        LiveAllocation *replaced = nullptr;
        bool inserted = false;
        const std::uint64_t hash = liveIndexHash(allocation->pointer);
        const std::size_t shard = liveIndexShard(hash);
        ::AcquireSRWLockExclusive(&live_index_locks[shard]);
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
        ::ReleaseSRWLockExclusive(&live_index_locks[shard]);

        if (replaced != nullptr) {
            // Reuse proves that the previous allocation ended even when its
            // deallocation bypassed the covered free entry points.
            retireAllocation(replaced, monotonicMs());
        }
        return inserted;
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
        const std::uint64_t current_tid = static_cast<std::uint64_t>(::GetCurrentThreadId());
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

        const std::uint64_t sample_points =
            consumeSampledBytes(state, requested_bytes, interval);
        if (sample_points == 0) {
            return;
        }
        sampling_points.fetch_add(sample_points, std::memory_order_relaxed);

        // Each sampling point represents one configured interval. Unlike the old
        // accumulated-byte scheme, bytes from preceding allocations are not
        // attributed to the allocation that happens to cross the threshold.
        const std::uint64_t weight = saturatingMultiply(sample_points, interval);

        PSLIST_ENTRY live_entry = ::InterlockedPopEntrySList(&free_live_allocations);
        if (live_entry == nullptr) {
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
        const std::uint64_t started = started_tick_ms.load(std::memory_order_relaxed);
        allocation->window = static_cast<std::int32_t>(
            (std::min)(
                static_cast<std::uint64_t>(kMaxProfileWindows - 1),
                started > 0 && allocation->allocated_ms >= started
                    ? (allocation->allocated_ms - started) / 1000
                    : 0));
        allocation->depth = static_cast<std::uint16_t>(::RtlCaptureStackBackTrace(
            kFramesToSkip, static_cast<ULONG>(kStackDepth), allocation->frames, nullptr));
        if (allocation->depth == 0 || !insertLiveAllocation(allocation)) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            ::InterlockedPushEntrySList(&free_live_allocations, &allocation->entry);
            return;
        }
        const std::uint64_t current_live =
            live_samples.fetch_add(1, std::memory_order_relaxed) + 1;
        live_bytes.fetch_add(weight, std::memory_order_relaxed);
        std::uint64_t previous_peak = peak_live_samples.load(std::memory_order_relaxed);
        while (previous_peak < current_live &&
               !peak_live_samples.compare_exchange_weak(
                   previous_peak, current_live, std::memory_order_relaxed)) {
        }

        if (config.live_only) {
            if (!thread.identity_announced) {
                PSLIST_ENTRY observation_entry =
                    ::InterlockedPopEntrySList(&free_events);
                if (observation_entry == nullptr) {
                    dropped_samples.fetch_add(1, std::memory_order_relaxed);
                    dropped_events.fetch_add(1, std::memory_order_relaxed);
                }
                else {
                    auto *observation =
                        CONTAINING_RECORD(observation_entry, AllocationEvent, entry);
                    observation->thread_id = allocation->thread_id;
                    observation->os_thread_id = allocation->os_thread_id;
                    observation->depth = 0;
                    observation->thread_observation = true;
                    const std::uint64_t ready =
                        ready_event_count.fetch_add(1, std::memory_order_relaxed) + 1;
                    std::uint64_t previous_high_water =
                        ready_event_high_water.load(std::memory_order_relaxed);
                    while (previous_high_water < ready &&
                           !ready_event_high_water.compare_exchange_weak(
                               previous_high_water, ready,
                               std::memory_order_relaxed)) {
                    }
                    ::InterlockedPushEntrySList(&ready_events,
                                                &observation->entry);
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
        event->weight_bytes = weight;
        event->tick_id = allocation->tick_id;
        event->thread_id = allocation->thread_id;
        event->os_thread_id = allocation->os_thread_id;
        event->window = allocation->window;
        event->depth = allocation->depth;
        event->thread_observation = false;
        std::memcpy(event->frames, allocation->frames,
                    static_cast<std::size_t>(allocation->depth) * sizeof(void *));
        const std::uint64_t ready =
            ready_event_count.fetch_add(1, std::memory_order_relaxed) + 1;
        std::uint64_t previous_high_water =
            ready_event_high_water.load(std::memory_order_relaxed);
        while (previous_high_water < ready &&
               !ready_event_high_water.compare_exchange_weak(
                   previous_high_water, ready, std::memory_order_relaxed)) {
        }
        ::InterlockedPushEntrySList(&ready_events, &event->entry);
        enqueued_samples.fetch_add(1, std::memory_order_relaxed);
    }

    bool allocateEventPool(std::string &error)
    {
        ::InitializeSListHead(&free_events);
        ::InitializeSListHead(&ready_events);
        ::InitializeSListHead(&free_live_allocations);
        const std::size_t bytes = sizeof(AllocationEvent) * kEventCapacity;
        event_storage = static_cast<AllocationEvent *>(
            ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (event_storage == nullptr) {
            error = "VirtualAlloc for allocation sample buffer failed: " + std::to_string(::GetLastError());
            return false;
        }
        live_storage = static_cast<LiveAllocation *>(::VirtualAlloc(
            nullptr, sizeof(LiveAllocation) * kEventCapacity,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        live_index = static_cast<LiveIndexEntry *>(::VirtualAlloc(
            nullptr, sizeof(LiveIndexEntry) * kLiveIndexCapacity,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (live_storage == nullptr || live_index == nullptr) {
            error = "VirtualAlloc for allocation lifecycle tracking failed: " +
                    std::to_string(::GetLastError());
            freeEventPool();
            return false;
        }
        for (std::size_t i = 0; i < kEventCapacity; ++i) {
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
    }

    void recycleEvent(AllocationEvent *event) noexcept
    {
        ::InterlockedPushEntrySList(&free_events, &event->entry);
    }

    std::string hookError(const char *operation, int code) const
    {
        std::string message(operation);
        message += " failed (code ";
        message += std::to_string(code);
        message += ")";
        if (hooks != nullptr) {
            const char *detail = funchook_error_message(hooks);
            if (detail != nullptr && *detail != '\0') {
                message += ": ";
                message += detail;
            }
        }
        return message;
    }

    template <typename Function>
    bool prepareExport(HMODULE module, const char *name, Function &function, void *hook,
                       bool required, std::string &error)
    {
        void *target = reinterpret_cast<void *>(::GetProcAddress(module, name));
        if (target == nullptr) {
            function = nullptr;
            hook_capabilities.push_back(
                {name, AllocationHookStatus::Missing, "export not found"});
            if (required) {
                error = std::string("required UCRT allocation export not found: ") + name;
                return false;
            }
            return true;
        }
        auto alias = std::find_if(prepared_targets.begin(), prepared_targets.end(),
                                  [target](const PreparedTarget &entry) {
                                      return entry.address == target;
                                  });
        if (alias != prepared_targets.end()) {
            // Some CRT exports are aliases for the same entry address. The first
            // prepared hook already covers all aliases; preparing the same prologue
            // twice would create an invalid hook chain.
            function = nullptr;
            hook_capabilities.push_back(
                {name, AllocationHookStatus::Alias, alias->export_name});
            return true;
        }

        PreparedTarget prepared;
        prepared.address = target;
        prepared.export_name = name;
        std::memcpy(prepared.original.data(), target, prepared.original.size());
        function = reinterpret_cast<Function>(target);
        const int code = funchook_prepare(hooks, reinterpret_cast<void **>(&function), hook);
        if (code != FUNCHOOK_ERROR_SUCCESS) {
            const std::string failure = hookError(
                (std::string("funchook_prepare(") + name + ")").c_str(), code);
            if (!required) {
                function = nullptr;
                hook_capabilities.push_back(
                    {name, AllocationHookStatus::PrepareFailed, failure});
                return true;
            }
            hook_capabilities.push_back(
                {name, AllocationHookStatus::PrepareFailed, failure});
            error = failure;
            return false;
        }
        prepared_targets.push_back(prepared);
        hook_capabilities.push_back({name, AllocationHookStatus::Active, {}});
        return true;
    }

    void addProtectedCodeRange(void *address)
    {
        if (address == nullptr) {
            return;
        }
        MEMORY_BASIC_INFORMATION memory{};
        if (::VirtualQuery(address, &memory, sizeof(memory)) == 0) {
            return;
        }
        CodeRange range;
        range.begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        range.end = range.begin + memory.RegionSize;
        if (std::none_of(protected_code_ranges.begin(), protected_code_ranges.end(),
                         [&range](const CodeRange &existing) {
                             return existing.begin == range.begin && existing.end == range.end;
                         })) {
            protected_code_ranges.push_back(range);
        }
    }

    bool rebuildProtectedCodeRanges(std::string &error)
    {
        protected_code_ranges.clear();

        HMODULE module = nullptr;
        const auto hook_address = reinterpret_cast<LPCWSTR>(
            reinterpret_cast<const void *>(&AllocationSampler::Impl::hookMalloc));
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 hook_address, &module) == FALSE) {
            error = "GetModuleHandleExW for Spark hook module failed: " +
                    std::to_string(::GetLastError());
            return false;
        }
        MODULEINFO module_info{};
        if (::GetModuleInformation(::GetCurrentProcess(), module, &module_info,
                                   sizeof(module_info)) == FALSE) {
            error = "GetModuleInformation for Spark hook module failed: " +
                    std::to_string(::GetLastError());
            return false;
        }
        protected_code_ranges.push_back(
            {reinterpret_cast<std::uintptr_t>(module_info.lpBaseOfDll),
             reinterpret_cast<std::uintptr_t>(module_info.lpBaseOfDll) + module_info.SizeOfImage});

        for (void *address : {
                 reinterpret_cast<void *>(real_malloc),
                 reinterpret_cast<void *>(real_calloc),
                 reinterpret_cast<void *>(real_realloc),
                 reinterpret_cast<void *>(real_recalloc),
                 reinterpret_cast<void *>(real_free),
                 reinterpret_cast<void *>(real_aligned_malloc),
                 reinterpret_cast<void *>(real_aligned_realloc),
                 reinterpret_cast<void *>(real_aligned_recalloc),
                 reinterpret_cast<void *>(real_aligned_offset_malloc),
                 reinterpret_cast<void *>(real_aligned_offset_realloc),
                 reinterpret_cast<void *>(real_aligned_offset_recalloc),
                 reinterpret_cast<void *>(real_aligned_free),
                 reinterpret_cast<void *>(real_malloc_base),
                 reinterpret_cast<void *>(real_calloc_base),
                 reinterpret_cast<void *>(real_realloc_base),
                 reinterpret_cast<void *>(real_free_base),
                 reinterpret_cast<void *>(real_heap_alloc),
                 reinterpret_cast<void *>(real_heap_realloc),
                 reinterpret_cast<void *>(real_heap_free),
             }) {
            addProtectedCodeRange(address);
        }
        return true;
    }

    bool restoreOriginalTargets(DWORD &first_failure) noexcept
    {
        first_failure = ERROR_SUCCESS;
        for (const PreparedTarget &target : prepared_targets) {
            DWORD old_protection = 0;
            if (::VirtualProtect(target.address, target.original.size(), PAGE_EXECUTE_READWRITE,
                                 &old_protection) == FALSE) {
                if (first_failure == ERROR_SUCCESS) {
                    first_failure = ::GetLastError();
                }
                continue;
            }
            std::memcpy(target.address, target.original.data(), target.original.size());
            ::FlushInstructionCache(::GetCurrentProcess(), target.address, target.original.size());
            DWORD ignored = 0;
            if (::VirtualProtect(target.address, target.original.size(), old_protection, &ignored) == FALSE &&
                first_failure == ERROR_SUCCESS) {
                first_failure = ::GetLastError();
            }
        }
        return first_failure == ERROR_SUCCESS;
    }

    bool prepareHooks(std::string &error)
    {
        if (hooks_prepared) {
            return true;
        }
        if (tls_index == TLS_OUT_OF_INDEXES) {
            tls_index = ::TlsAlloc();
            if (tls_index == TLS_OUT_OF_INDEXES) {
                error = "TlsAlloc for allocation thread state failed: " +
                        std::to_string(::GetLastError());
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

        hooks = funchook_create();
        if (hooks == nullptr) {
            error = "funchook_create failed";
            return false;
        }

        bool ok =
            prepareExport(ucrt, "malloc", real_malloc, reinterpret_cast<void *>(&hookMalloc), true, error) &&
            prepareExport(ucrt, "calloc", real_calloc, reinterpret_cast<void *>(&hookCalloc), true, error) &&
            prepareExport(ucrt, "realloc", real_realloc, reinterpret_cast<void *>(&hookRealloc), true, error) &&
            prepareExport(ucrt, "_recalloc", real_recalloc, reinterpret_cast<void *>(&hookRecalloc), false, error) &&
            prepareExport(ucrt, "free", real_free, reinterpret_cast<void *>(&hookFree), true, error) &&
            prepareExport(ucrt, "_aligned_malloc", real_aligned_malloc,
                          reinterpret_cast<void *>(&hookAlignedMalloc), false, error) &&
            prepareExport(ucrt, "_aligned_realloc", real_aligned_realloc,
                          reinterpret_cast<void *>(&hookAlignedRealloc), false, error) &&
            prepareExport(ucrt, "_aligned_recalloc", real_aligned_recalloc,
                          reinterpret_cast<void *>(&hookAlignedRecalloc), false, error) &&
            prepareExport(ucrt, "_aligned_offset_malloc", real_aligned_offset_malloc,
                          reinterpret_cast<void *>(&hookAlignedOffsetMalloc), false, error) &&
            prepareExport(ucrt, "_aligned_offset_realloc", real_aligned_offset_realloc,
                          reinterpret_cast<void *>(&hookAlignedOffsetRealloc), false, error) &&
            prepareExport(ucrt, "_aligned_offset_recalloc", real_aligned_offset_recalloc,
                          reinterpret_cast<void *>(&hookAlignedOffsetRecalloc), false, error) &&
            prepareExport(ucrt, "_aligned_free", real_aligned_free,
                          reinterpret_cast<void *>(&hookAlignedFree), true, error) &&
            prepareExport(ucrt, "_malloc_base", real_malloc_base,
                          reinterpret_cast<void *>(&hookMallocBase), false, error) &&
            prepareExport(ucrt, "_calloc_base", real_calloc_base,
                          reinterpret_cast<void *>(&hookCallocBase), false, error) &&
            prepareExport(ucrt, "_realloc_base", real_realloc_base,
                          reinterpret_cast<void *>(&hookReallocBase), false, error) &&
            prepareExport(ucrt, "_free_base", real_free_base,
                          reinterpret_cast<void *>(&hookFreeBase), false, error) &&
            prepareExport(kernel32, "HeapAlloc", real_heap_alloc,
                          reinterpret_cast<void *>(&hookHeapAlloc), false, error) &&
            prepareExport(kernel32, "HeapReAlloc", real_heap_realloc,
                          reinterpret_cast<void *>(&hookHeapReAlloc), false, error) &&
            prepareExport(kernel32, "HeapFree", real_heap_free,
                          reinterpret_cast<void *>(&hookHeapFree), true, error);

        if (!ok) {
            funchook_destroy(hooks);
            hooks = nullptr;
            clearFunctionPointers();
            return false;
        }

        if (!rebuildProtectedCodeRanges(error)) {
            funchook_destroy(hooks);
            hooks = nullptr;
            clearFunctionPointers();
            return false;
        }

        Impl *expected = nullptr;
        if (!active_instance.compare_exchange_strong(expected, this, std::memory_order_release,
                                                     std::memory_order_relaxed) && expected != this) {
            error = "another native allocation sampler backend is already active";
            funchook_destroy(hooks);
            hooks = nullptr;
            clearFunctionPointers();
            return false;
        }

        hooks_prepared = true;
        return true;
    }

    bool installHooks(std::string &error)
    {
        if (hooks_installed.load(std::memory_order_acquire)) {
            return true;
        }
        if (hook_state_unknown) {
            error = "allocation hook state is unknown after an earlier lifecycle failure";
            return false;
        }
        SuspendedProcessThreads suspended;
        if (!suspended.suspendStable(error)) {
            return false;
        }

        const int code = funchook_install(hooks, 0);
        DWORD rollback_failure = ERROR_SUCCESS;
        const bool rollback_ok =
            code == FUNCHOOK_ERROR_SUCCESS || restoreOriginalTargets(rollback_failure);
        std::string resume_error;
        const bool resumed = suspended.resume(resume_error);
        if (code != FUNCHOOK_ERROR_SUCCESS) {
            error = hookError("funchook_install", code);
            if (!rollback_ok) {
                hook_state_unknown = true;
                hooks_installed.store(true, std::memory_order_release);
                error += "; restoring partially installed entry hooks failed: " +
                         std::to_string(rollback_failure);
            }
            if (!resume_error.empty()) {
                error += "; " + resume_error;
            }
            return false;
        }
        hooks_installed.store(true, std::memory_order_release);
        if (!resumed) {
            error = resume_error;
            return false;
        }
        return true;
    }

    static bool anyActiveHookCalls() noexcept
    {
        return std::any_of(
            active_hook_calls.begin(), active_hook_calls.end(),
            [](const HookCounter &counter) {
                return counter.value.load(std::memory_order_acquire) != 0;
            });
    }

    bool uninstallHooks(std::string &error)
    {
        if (!hooks_installed.load(std::memory_order_acquire)) {
            return true;
        }

        SuspendedProcessThreads suspended;
        if (!suspended.suspendStable(error)) {
            return false;
        }

        bool instruction_in_protected_code = false;
        DWORD inspect_failure = ERROR_SUCCESS;
        DWORD inspect_thread = 0;
        const bool inspected = suspended.anyInstructionPointerInRanges(
            protected_code_ranges, instruction_in_protected_code,
            inspect_failure, inspect_thread);
        const bool active_calls = anyActiveHookCalls();
        if (!inspected || instruction_in_protected_code || active_calls) {
            std::string resume_error;
            const bool resumed = suspended.resume(resume_error);
            if (!inspected) {
                error = "GetThreadContext failed for thread " +
                        std::to_string(inspect_thread) + ": " +
                        std::to_string(inspect_failure);
            }
            else {
                error = "allocation hook calls are still active during shutdown";
            }
            if (!resumed) {
                error += "; " + resume_error;
            }
            return false;
        }

        const int code = funchook_uninstall(hooks, 0);
        DWORD rollback_failure = ERROR_SUCCESS;
        const bool restored =
            code == FUNCHOOK_ERROR_SUCCESS || restoreOriginalTargets(rollback_failure);
        std::string resume_error;
        const bool resumed = suspended.resume(resume_error);
        if (code == FUNCHOOK_ERROR_SUCCESS) {
            hooks_installed.store(false, std::memory_order_release);
            hook_state_unknown = false;
        }
        if (code != FUNCHOOK_ERROR_SUCCESS) {
            error = hookError("funchook_uninstall", code);
            if (!restored) {
                hook_state_unknown = true;
                error += "; restoring allocator entry bytes failed: " +
                         std::to_string(rollback_failure);
            }
            else if (hook_state_unknown && code == FUNCHOOK_ERROR_NOT_INSTALLED) {
                // A failed install can leave funchook's internal installed flag
                // clear even though an entry write partially succeeded. Manual
                // byte restoration is authoritative in that state.
                hooks_installed.store(false, std::memory_order_release);
            }
            if (!resume_error.empty()) {
                error += "; " + resume_error;
            }
            return false;
        }
        if (!resumed) {
            error = resume_error;
            return false;
        }
        return true;
    }

    bool waitForQuiescence(std::string &error)
    {
        const std::uint64_t deadline = monotonicMs() + 30000;
        while (true) {
            SuspendedProcessThreads suspended;
            if (!suspended.suspendStable(error)) {
                return false;
            }
            bool instruction_in_protected_code = false;
            DWORD inspect_failure = ERROR_SUCCESS;
            DWORD inspect_thread = 0;
            const bool inspected = suspended.anyInstructionPointerInRanges(
                protected_code_ranges, instruction_in_protected_code, inspect_failure,
                inspect_thread);
            const bool active_calls = anyActiveHookCalls();
            std::string resume_error;
            const bool resumed = suspended.resume(resume_error);
            if (!inspected) {
                if (!resumed) {
                    error = resume_error;
                    return false;
                }
                if (monotonicMs() >= deadline) {
                    error =
                        "timed out waiting for stable thread contexts; "
                        "GetThreadContext failed for thread " +
                        std::to_string(inspect_thread) + ": " +
                        std::to_string(inspect_failure);
                    return false;
                }
                ::Sleep(1);
                continue;
            }
            if (!resumed) {
                error = resume_error;
                return false;
            }
            if (!instruction_in_protected_code && !active_calls) {
                return true;
            }
            if (monotonicMs() >= deadline) {
                error = "timed out waiting for allocation hook/trampoline calls to leave the plugin";
                return false;
            }
            ::Sleep(1);
        }
    }

    bool destroyHooks(std::string &error)
    {
        if (hooks == nullptr) {
            releaseThreadStateRegistry();
            return true;
        }
        if (hooks_installed.load(std::memory_order_acquire)) {
            error = "cannot destroy allocation hook trampolines while entry hooks are installed";
            return false;
        }

        if (!waitForQuiescence(error)) {
            return false;
        }
        const int code = funchook_destroy(hooks);
        if (code != FUNCHOOK_ERROR_SUCCESS) {
            error = "funchook_destroy failed (code " + std::to_string(code) + ")";
            return false;
        }
        hooks = nullptr;
        hooks_prepared = false;
        hook_state_unknown = false;
        Impl *expected = this;
        active_instance.compare_exchange_strong(expected, nullptr, std::memory_order_release,
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
        prepared_targets.clear();
        protected_code_ranges.clear();
        hook_capabilities.clear();
    }

    FrameKey frameKeyForAddress(std::uint64_t raw_address, std::string &module_path)
    {
        MEMORY_BASIC_INFORMATION memory{};
        std::uintptr_t module_base = 0;
        if (::VirtualQuery(reinterpret_cast<void *>(static_cast<std::uintptr_t>(raw_address)), &memory,
                           sizeof(memory)) != 0) {
            module_base = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
        }

        ModuleId module_id = kInvalidModule;
        auto cache = module_cache.find(module_base);
        if (cache != module_cache.end()) {
            module_id = cache->second;
            module_path = modules.path(module_id);
        }
        else {
            char path[MAX_PATH]{};
            const DWORD length = module_base != 0
                                     ? ::GetModuleFileNameA(reinterpret_cast<HMODULE>(module_base), path,
                                                            static_cast<DWORD>(sizeof(path)))
                                     : 0;
            module_path = length > 0 ? std::string(path, length) : std::string("unknown");
            const std::size_t prev_module_count = modules.size();
            module_id = modules.intern(module_path);
            if (recovery_sink_ && modules.size() > prev_module_count) {
                recovery_sink_->journalModuleDef(module_id, module_path);
            }
            if (module_cache.size() < kMaxModuleCacheEntries) {
                module_cache.emplace(module_base, module_id);
            }
        }

        FrameKey key;
        key.module = module_id;
        key.rva = module_base != 0 ? raw_address - module_base : raw_address;
        key.raw_address = raw_address;
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

        if (recovery_sink_) {
            if (journaled_threads_.insert(sample.thread_id).second) {
                recovery_sink_->journalThreadDef(sample.thread_id, sample.thread_id,
                                                 sample.thread_name);
            }
            recovery_sink_->journalSample(sample);
        }
    }

    void flushOrDrop(std::uint64_t tick_id, bool keep)
    {
        auto it = buckets.find(tick_id);
        if (it == buckets.end()) {
            return;
        }
        if (keep) {
            for (const Sample &sample : it->second) {
                acceptSample(sample);
            }
        }
        pending_samples -= it->second.size();
        buckets.erase(it);
    }

    bool buildSample(void *const *frames, std::uint16_t depth, std::uint64_t tick_id,
                     std::uint64_t thread_id, std::uint64_t os_thread_id,
                     std::int32_t window, std::uint64_t weight, Sample &sample)
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

        bool leading = true;
        for (std::size_t i = 0; i < depth; ++i) {
            const std::uint64_t raw = reinterpret_cast<std::uint64_t>(frames[i]);
            if (raw == 0) {
                continue;
            }
            std::string path;
            FrameKey key = frameKeyForAddress(raw, path);
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

    bool tickAccepts(std::uint64_t tick_id) const noexcept
    {
        if (config.only_ticks_over_ms <= 0) {
            return true;
        }
        return tick_id < tick_decisions.size() &&
               tick_decisions[static_cast<std::size_t>(tick_id)] == 2;
    }

    void processEvent(AllocationEvent *event)
    {
        if (event->thread_observation) {
            thread_filter.resolve(event->thread_id, event->os_thread_id);
            return;
        }
        Sample sample;
        if (!buildSample(event->frames, event->depth, event->tick_id,
                         event->thread_id, event->os_thread_id, event->window,
                         event->weight_bytes, sample)) {
            return;
        }

        const bool ticked = config.only_ticks_over_ms > 0;
        if (!ticked) {
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

    void drainQueues()
    {
        const bool ticked = config.only_ticks_over_ms > 0;
        const double threshold = static_cast<double>(config.only_ticks_over_ms);

        TickEvent tick;
        while (ticks.dequeue(tick)) {
            const bool keep = !ticked || tick.mspt_ms > threshold;
            if (ticked && tick.tick_id < kMaxTickDecisions) {
                if (tick_decisions.size() <= tick.tick_id) {
                    tick_decisions.resize(static_cast<std::size_t>(tick.tick_id + 1), 0);
                }
                tick_decisions[static_cast<std::size_t>(tick.tick_id)] = keep ? 2 : 1;
            }
            if (recovery_sink_) {
                recovery_sink_->journalTickEvent(tick.tick_id, tick.mspt_ms);
            }
            flushOrDrop(tick.tick_id, keep);
        }

        PSLIST_ENTRY list = ::InterlockedFlushSList(&ready_events);
        while (list != nullptr) {
            PSLIST_ENTRY next = list->Next;
            auto *event = CONTAINING_RECORD(list, AllocationEvent, entry);
            processEvent(event);
            ready_event_count.fetch_sub(1, std::memory_order_relaxed);
            recycleEvent(event);
            list = next;
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
        module_cache.clear();
        journaled_threads_.clear();
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
        ready_event_count.store(0, std::memory_order_relaxed);
        ready_event_high_water.store(0, std::memory_order_relaxed);
        tracking_hook_calls.store(0, std::memory_order_relaxed);
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

    bool waitForTrackingQuiescence(std::string &error) noexcept
    {
        for (int attempt = 0; attempt < 5000; ++attempt) {
            if (tracking_hook_calls.load(std::memory_order_acquire) == 0) {
                return true;
            }
            ::Sleep(1);
        }
        try {
            error = "timed out waiting for allocation lifecycle hooks to quiesce";
        }
        catch (...) {
        }
        return false;
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
        if (hook_state_unknown) {
            error = "allocation hook state is unknown after an earlier lifecycle failure";
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

        resetSession();
        config = new_config;
        if (!thread_filter.configure(new_config.all_threads,
                                     new_config.regex_threads,
                                     new_config.thread_patterns, error)) {
            return false;
        }
        interval_bytes.store(static_cast<std::uint64_t>(new_config.interval_bytes), std::memory_order_relaxed);
        started_tick_ms.store(monotonicMs(), std::memory_order_relaxed);
        const std::uint64_t new_generation = generation.fetch_add(1, std::memory_order_relaxed) + 1;
        sampling_seed.store(new_generation ^ monotonicMs() ^ new_config.session_seed,
                            std::memory_order_relaxed);

        if (!prepareHooks(error) || !allocateEventPool(error)) {
            return false;
        }

        if (!installHooks(error)) {
            freeEventPool();
            return false;
        }

        aggregator_running.store(true, std::memory_order_release);
        running.store(true, std::memory_order_release);
        tracking.store(true, std::memory_order_release);
        try {
            TrackingSuppressionGuard suppress(*this);
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
            });
        }
        catch (...) {
            tracking.store(false, std::memory_order_release);
            running.store(false, std::memory_order_release);
            aggregator_running.store(false, std::memory_order_release);
            std::string quiescence_error;
            if (!waitForTrackingQuiescence(quiescence_error)) {
                error = std::move(quiescence_error);
                return false;
            }
            freeEventPool();
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
        if (!waitForTrackingQuiescence(error)) {
            return false;
        }
        aggregator_running.store(false, std::memory_order_release);
        if (aggregator_thread.joinable()) {
            aggregator_thread.join();
        }
        if (config.live_only && !aggregator_failed.load(std::memory_order_acquire)) {
            finalizeLiveProfile();
        }
        freeEventPool();
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
        if (!waitForTrackingQuiescence(error)) {
            return false;
        }
        aggregator_running.store(false, std::memory_order_release);
        if (aggregator_thread.joinable()) {
            aggregator_thread.join();
        }
        freeEventPool();

        if (hooks_installed.load(std::memory_order_acquire)) {
            std::string last_error;
            const std::uint64_t uninstall_deadline = monotonicMs() + 30000;
            while (hooks_installed.load(std::memory_order_acquire)) {
                if (!uninstallHooks(last_error)) {
                    // A thread can be observed while it is starting or exiting:
                    // SuspendThread succeeds but GetThreadContext temporarily
                    // reports ERROR_GEN_FAILURE. Resume every thread and retry a
                    // fresh stable snapshot until the same bounded shutdown
                    // deadline used for trampoline quiescence.
                    if (monotonicMs() >= uninstall_deadline) {
                        break;
                    }
                    ::Sleep(1);
                }
            }
            if (hooks_installed.load(std::memory_order_acquire)) {
                error = "could not remove allocation hooks during shutdown: " + last_error;
                return false;
            }
        }

        return destroyHooks(error);
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

        const std::uint64_t started = started_tick_ms.load(std::memory_order_relaxed);
        const std::uint64_t now = monotonicMs();
        const std::int32_t window =
            static_cast<std::int32_t>((std::min)(
                static_cast<std::uint64_t>(kMaxProfileWindows - 1),
                started > 0 && now >= started ? (now - started) / 1000 : 0));
        WindowTickStats &stats = window_ticks[window];
        stats.ticks += 1;
        stats.mspt_sum += mspt_ms;
        stats.mspt_max = (std::max)(stats.mspt_max, mspt_ms);
    }
};

std::atomic<AllocationSampler::Impl *> AllocationSampler::Impl::active_instance{nullptr};
std::array<AllocationSampler::Impl::HookCounter, 64>
    AllocationSampler::Impl::active_hook_calls{};

AllocationSampler::AllocationSampler() : impl_(std::make_unique<Impl>())
{
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
    // Unloading this DLL with an unproven hook/trampoline state is never safe.
    // Do not pin or leak old plugin code: fail closed so the process cannot
    // continue into a reload with stale allocator entry points.
    std::abort();
}

bool AllocationSampler::start(const AllocationSamplerConfig &config, std::string &error)
{
    return impl_->startSession(config, error);
}

void AllocationSampler::setRecoverySink(RecoverySink *sink)
{
    impl_->recovery_sink_ = sink;
}

bool AllocationSampler::stop(std::string &error)
{
    return impl_->stopSession(error);
}

bool AllocationSampler::shutdown(std::string &error)
{
    return impl_->shutdownBackend(error);
}

void AllocationSampler::onTick(double mspt_ms)
{
    impl_->tick(mspt_ms);
}

const CallTree &AllocationSampler::tree() const
{
    return impl_->tree;
}

const std::map<std::uint64_t, ThreadCallTree> &AllocationSampler::threadTrees() const
{
    return impl_->thread_trees;
}

const ModuleTable &AllocationSampler::modules() const
{
    return impl_->modules;
}

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
    return impl_->ready_event_high_water.load(std::memory_order_relaxed);
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
    return impl_->hooks_prepared ? 2 : 0;
}

std::uint64_t AllocationSampler::skippedModuleCount() const
{
    return 0;
}

std::uint64_t AllocationSampler::failedModuleCount() const
{
    return 0;
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
           impl_->profile_nodes_exhausted.load(std::memory_order_relaxed);
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

const std::vector<AllocationHookCapability> &AllocationSampler::hookCapabilities() const
{
    return impl_->hook_capabilities;
}

std::size_t AllocationSampler::hookTargetCount() const
{
    return impl_->prepared_targets.size();
}

}  // namespace spark
