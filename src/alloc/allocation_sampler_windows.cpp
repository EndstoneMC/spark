#include "alloc/allocation_sampler.h"

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
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <funchook.h>
#include <moodycamel/concurrentqueue.h>

#include "alloc/byte_sampler.h"

namespace spark {
namespace {

constexpr std::size_t kStackDepth = 48;
constexpr std::size_t kEventCapacity = 16384;
constexpr std::size_t kLiveIndexCapacity = kEventCapacity * 2;
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
        std::int32_t window = 0;
        std::uint16_t depth = 0;
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

    static std::atomic<Impl *> active_instance;
    static std::atomic<std::uint64_t> active_hook_calls;
    static thread_local bool inside_hook;
    static thread_local bool tracking_suppressed;
    static thread_local ByteSamplingState thread_sampling;

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

        HookCallGuard(const HookCallGuard &) = delete;
        HookCallGuard &operator=(const HookCallGuard &) = delete;
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
        RecursionGuard() noexcept : owner_(!inside_hook)
        {
            if (owner_) {
                inside_hook = true;
            }
        }

        ~RecursionGuard()
        {
            if (owner_) {
                inside_hook = false;
            }
        }

        bool owner() const noexcept
        {
            return owner_;
        }

    private:
        bool owner_;
    };

    class TrackingSuppressionGuard {
    public:
        TrackingSuppressionGuard() noexcept : previous_(tracking_suppressed)
        {
            tracking_suppressed = true;
        }

        ~TrackingSuppressionGuard()
        {
            tracking_suppressed = previous_;
        }

    private:
        bool previous_;
    };

    funchook_t *hooks = nullptr;
    bool hooks_prepared = false;
    bool hook_state_unknown = false;
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
    std::atomic<std::uint64_t> target_tid{0};
    std::atomic<std::uint64_t> current_tick{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> interval_bytes{kDefaultAllocationIntervalBytes};
    std::atomic<std::uint64_t> sampling_seed{0};
    std::atomic<std::uint64_t> started_tick_ms{0};
    std::atomic<std::uint64_t> sample_count{0};
    std::atomic<std::uint64_t> sampled_bytes{0};
    std::atomic<std::uint64_t> observed_bytes{0};
    std::atomic<std::uint64_t> dropped_samples{0};
    std::atomic<std::uint64_t> tracking_hook_calls{0};
    std::atomic<std::uint64_t> next_allocation_id{1};
    std::atomic<std::uint64_t> freed_samples{0};
    std::atomic<std::uint64_t> freed_bytes{0};
    std::atomic<std::uint64_t> live_samples{0};
    std::atomic<std::uint64_t> live_bytes{0};
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
    SRWLOCK live_index_lock = SRWLOCK_INIT;

    std::thread aggregator_thread;
    moodycamel::ConcurrentQueue<TickEvent> ticks;

    CallTree tree;
    ModuleTable modules;
    std::map<std::int32_t, WindowTickStats> window_ticks;
    std::unordered_map<std::uint64_t, std::vector<Sample>> buckets;
    std::vector<std::uint8_t> tick_decisions;
    std::unordered_map<std::uintptr_t, ModuleId> module_cache;

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

    bool shouldTrackCurrentThread() const noexcept
    {
        if (!tracking.load(std::memory_order_relaxed) ||
            ::GetCurrentThreadId() !=
                static_cast<DWORD>(target_tid.load(std::memory_order_relaxed))) {
            return false;
        }
        // TLS access can itself allocate while Windows is constructing a new
        // thread's TLS vector. Only the already-established target server
        // thread may reach this read; all other threads stay on the TLS-free
        // pass-through path.
        return !tracking_suppressed;
    }

    static std::size_t liveIndexSlot(void *pointer) noexcept
    {
        const auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer) >> 4);
        return static_cast<std::size_t>((value * 11400714819323198485ull) &
                                        (kLiveIndexCapacity - 1));
    }

    std::uint64_t lookupAllocationId(void *pointer) noexcept
    {
        if (pointer == nullptr || live_index == nullptr) {
            return 0;
        }
        ::AcquireSRWLockShared(&live_index_lock);
        const std::size_t start = liveIndexSlot(pointer);
        std::uint64_t result = 0;
        for (std::size_t offset = 0; offset < kLiveIndexCapacity; ++offset) {
            const LiveIndexEntry &entry = live_index[(start + offset) & (kLiveIndexCapacity - 1)];
            if (entry.pointer == nullptr) {
                break;
            }
            if (entry.pointer == pointer) {
                result = entry.allocation_id;
                break;
            }
        }
        ::ReleaseSRWLockShared(&live_index_lock);
        return result;
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

    bool releaseAllocation(void *pointer, std::uint64_t expected_id) noexcept
    {
        if (pointer == nullptr || expected_id == 0 || live_index == nullptr) {
            return false;
        }
        LiveAllocation *released = nullptr;
        ::AcquireSRWLockExclusive(&live_index_lock);
        const std::size_t start = liveIndexSlot(pointer);
        for (std::size_t offset = 0; offset < kLiveIndexCapacity; ++offset) {
            LiveIndexEntry &entry = live_index[(start + offset) & (kLiveIndexCapacity - 1)];
            if (entry.pointer == nullptr) {
                break;
            }
            if (entry.pointer == pointer && entry.allocation_id == expected_id) {
                released = entry.allocation;
                entry.pointer = reinterpret_cast<void *>(static_cast<std::uintptr_t>(1));
                entry.allocation_id = 0;
                entry.allocation = nullptr;
                break;
            }
        }
        ::ReleaseSRWLockExclusive(&live_index_lock);
        if (released != nullptr) {
            retireAllocation(released, monotonicMs());
            return true;
        }
        return false;
    }

    void handleFree(FreeFn function, void *pointer) noexcept
    {
        TrackingCallGuard tracking_call(*this);
        const std::uint64_t allocation_id = tracking_call ? lookupAllocationId(pointer) : 0;
        function(pointer);
        if (tracking_call) {
            releaseAllocation(pointer, allocation_id);
        }
    }

    BOOL handleHeapFree(HeapFreeFn function, HANDLE heap, DWORD flags, void *pointer) noexcept
    {
        TrackingCallGuard tracking_call(*this);
        const std::uint64_t allocation_id = tracking_call ? lookupAllocationId(pointer) : 0;
        const BOOL result = function(heap, flags, pointer);
        if (tracking_call && result != FALSE) {
            releaseAllocation(pointer, allocation_id);
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
        RecursionGuard recursion;
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
        RecursionGuard recursion;
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
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, requested_size);
        }
        const bool target_thread = ::GetCurrentThreadId() ==
                                   static_cast<DWORD>(target_tid.load(std::memory_order_relaxed));
        if (!target_thread) {
            const std::uint64_t previous_id = lookupAllocationId(pointer);
            void *new_pointer = function(pointer, requested_size);
            if (new_pointer != nullptr || (pointer != nullptr && requested_size == 0)) {
                releaseAllocation(pointer, previous_id);
            }
            return new_pointer;
        }
        RecursionGuard recursion;
        if (!recursion.owner()) {
            return function(pointer, requested_size);
        }
        const std::uint64_t previous_id = lookupAllocationId(pointer);
        void *new_pointer = function(pointer, requested_size);
        if (new_pointer != nullptr || (pointer != nullptr && requested_size == 0)) {
            releaseAllocation(pointer, previous_id);
        }
        if (target_thread && new_pointer != nullptr && requested_size != 0) {
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
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, count, requested_size);
        }
        const bool target_thread = ::GetCurrentThreadId() ==
                                   static_cast<DWORD>(target_tid.load(std::memory_order_relaxed));
        if (!target_thread) {
            std::uint64_t bytes = 0;
            const bool valid_size = checkedMultiply(count, requested_size, bytes);
            const std::uint64_t previous_id = lookupAllocationId(pointer);
            void *new_pointer = function(pointer, count, requested_size);
            if (new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0)) {
                releaseAllocation(pointer, previous_id);
            }
            return new_pointer;
        }
        RecursionGuard recursion;
        if (!recursion.owner()) {
            return function(pointer, count, requested_size);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, requested_size, bytes);
        const std::uint64_t previous_id = lookupAllocationId(pointer);
        void *new_pointer = function(pointer, count, requested_size);
        if (new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0)) {
            releaseAllocation(pointer, previous_id);
        }
        if (target_thread && new_pointer != nullptr && valid_size && bytes != 0) {
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
        RecursionGuard recursion;
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
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, size, alignment);
        }
        const bool target_thread = ::GetCurrentThreadId() ==
                                   static_cast<DWORD>(target_tid.load(std::memory_order_relaxed));
        if (!target_thread) {
            const std::uint64_t previous_id = lookupAllocationId(pointer);
            void *new_pointer = function(pointer, size, alignment);
            if (new_pointer != nullptr || (pointer != nullptr && size == 0)) {
                releaseAllocation(pointer, previous_id);
            }
            return new_pointer;
        }
        RecursionGuard recursion;
        if (!recursion.owner()) {
            return function(pointer, size, alignment);
        }
        const std::uint64_t previous_id = lookupAllocationId(pointer);
        void *new_pointer = function(pointer, size, alignment);
        if (new_pointer != nullptr || (pointer != nullptr && size == 0)) {
            releaseAllocation(pointer, previous_id);
        }
        if (target_thread && new_pointer != nullptr && size != 0) {
            recordAllocation(new_pointer, static_cast<std::uint64_t>(size));
        }
        return new_pointer;
    }

    void *handleAlignedRecalloc(AlignedRecallocFn function, void *pointer, std::size_t count,
                                std::size_t size, std::size_t alignment) noexcept
    {
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, count, size, alignment);
        }
        const bool target_thread = ::GetCurrentThreadId() ==
                                   static_cast<DWORD>(target_tid.load(std::memory_order_relaxed));
        if (!target_thread) {
            std::uint64_t bytes = 0;
            const bool valid_size = checkedMultiply(count, size, bytes);
            const std::uint64_t previous_id = lookupAllocationId(pointer);
            void *new_pointer = function(pointer, count, size, alignment);
            if (new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0)) {
                releaseAllocation(pointer, previous_id);
            }
            return new_pointer;
        }
        RecursionGuard recursion;
        if (!recursion.owner()) {
            return function(pointer, count, size, alignment);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, size, bytes);
        const std::uint64_t previous_id = lookupAllocationId(pointer);
        void *new_pointer = function(pointer, count, size, alignment);
        if (new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0)) {
            releaseAllocation(pointer, previous_id);
        }
        if (target_thread && new_pointer != nullptr && valid_size && bytes != 0) {
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
        RecursionGuard recursion;
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
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, size, alignment, offset);
        }
        const bool target_thread = ::GetCurrentThreadId() ==
                                   static_cast<DWORD>(target_tid.load(std::memory_order_relaxed));
        if (!target_thread) {
            const std::uint64_t previous_id = lookupAllocationId(pointer);
            void *new_pointer = function(pointer, size, alignment, offset);
            if (new_pointer != nullptr || (pointer != nullptr && size == 0)) {
                releaseAllocation(pointer, previous_id);
            }
            return new_pointer;
        }
        RecursionGuard recursion;
        if (!recursion.owner()) {
            return function(pointer, size, alignment, offset);
        }
        const std::uint64_t previous_id = lookupAllocationId(pointer);
        void *new_pointer = function(pointer, size, alignment, offset);
        if (new_pointer != nullptr || (pointer != nullptr && size == 0)) {
            releaseAllocation(pointer, previous_id);
        }
        if (target_thread && new_pointer != nullptr && size != 0) {
            recordAllocation(new_pointer, static_cast<std::uint64_t>(size));
        }
        return new_pointer;
    }

    void *handleAlignedOffsetRecalloc(AlignedOffsetRecallocFn function, void *pointer, std::size_t count,
                                      std::size_t size, std::size_t alignment, std::size_t offset) noexcept
    {
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, count, size, alignment, offset);
        }
        const bool target_thread = ::GetCurrentThreadId() ==
                                   static_cast<DWORD>(target_tid.load(std::memory_order_relaxed));
        if (!target_thread) {
            std::uint64_t bytes = 0;
            const bool valid_size = checkedMultiply(count, size, bytes);
            const std::uint64_t previous_id = lookupAllocationId(pointer);
            void *new_pointer = function(pointer, count, size, alignment, offset);
            if (new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0)) {
                releaseAllocation(pointer, previous_id);
            }
            return new_pointer;
        }
        RecursionGuard recursion;
        if (!recursion.owner()) {
            return function(pointer, count, size, alignment, offset);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, size, bytes);
        const std::uint64_t previous_id = lookupAllocationId(pointer);
        void *new_pointer = function(pointer, count, size, alignment, offset);
        if (new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0)) {
            releaseAllocation(pointer, previous_id);
        }
        if (target_thread && new_pointer != nullptr && valid_size && bytes != 0) {
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
        RecursionGuard recursion;
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
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(heap, flags, pointer, requested_size);
        }
        const bool target_thread = ::GetCurrentThreadId() ==
                                   static_cast<DWORD>(target_tid.load(std::memory_order_relaxed));
        if (!target_thread) {
            const std::uint64_t previous_id = lookupAllocationId(pointer);
            void *new_pointer = function(heap, flags, pointer, requested_size);
            if (new_pointer != nullptr) {
                releaseAllocation(pointer, previous_id);
            }
            return new_pointer;
        }
        RecursionGuard recursion;
        if (!recursion.owner()) {
            return function(heap, flags, pointer, requested_size);
        }
        const std::uint64_t previous_id = lookupAllocationId(pointer);
        void *new_pointer = function(heap, flags, pointer, requested_size);
        if (new_pointer != nullptr) {
            releaseAllocation(pointer, previous_id);
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
        ::AcquireSRWLockExclusive(&live_index_lock);
        const std::size_t start = liveIndexSlot(allocation->pointer);
        std::size_t tombstone = kLiveIndexCapacity;
        for (std::size_t offset = 0; offset < kLiveIndexCapacity; ++offset) {
            const std::size_t slot = (start + offset) & (kLiveIndexCapacity - 1);
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
        ::ReleaseSRWLockExclusive(&live_index_lock);

        if (replaced != nullptr) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            live_samples.fetch_sub(1, std::memory_order_relaxed);
            live_bytes.fetch_sub(replaced->weight_bytes, std::memory_order_relaxed);
            ::InterlockedPushEntrySList(&free_live_allocations, &replaced->entry);
        }
        return inserted;
    }

    void recordAllocation(void *pointer, std::uint64_t requested_bytes) noexcept
    {
        if (requested_bytes == 0) {
            return;
        }
        observed_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);

        ByteSamplingState &state = thread_sampling;
        const std::uint64_t current_generation = generation.load(std::memory_order_relaxed);
        const std::uint64_t interval = interval_bytes.load(std::memory_order_relaxed);
        if (state.generation != current_generation) {
            resetByteSamplingState(
                state, current_generation,
                sampling_seed.load(std::memory_order_relaxed) ^ current_generation ^
                    static_cast<std::uint64_t>(::GetCurrentThreadId()),
                interval);
        }

        const std::uint64_t sample_points =
            consumeSampledBytes(state, requested_bytes, interval);
        if (sample_points == 0) {
            return;
        }

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
        const std::uint64_t started = started_tick_ms.load(std::memory_order_relaxed);
        allocation->window = static_cast<std::int32_t>(
            started > 0 && allocation->allocated_ms >= started
                ? (allocation->allocated_ms - started) / 1000
                : 0);
        allocation->depth = static_cast<std::uint16_t>(::RtlCaptureStackBackTrace(
            kFramesToSkip, static_cast<ULONG>(kStackDepth), allocation->frames, nullptr));
        if (allocation->depth == 0 || !insertLiveAllocation(allocation)) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            ::InterlockedPushEntrySList(&free_live_allocations, &allocation->entry);
            return;
        }
        live_samples.fetch_add(1, std::memory_order_relaxed);
        live_bytes.fetch_add(weight, std::memory_order_relaxed);

        if (config.live_only) {
            return;
        }

        PSLIST_ENTRY entry = ::InterlockedPopEntrySList(&free_events);
        if (entry == nullptr) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto *event = CONTAINING_RECORD(entry, AllocationEvent, entry);
        event->weight_bytes = weight;
        event->tick_id = allocation->tick_id;
        event->window = allocation->window;
        event->depth = allocation->depth;
        std::memcpy(event->frames, allocation->frames,
                    static_cast<std::size_t>(allocation->depth) * sizeof(void *));
        ::InterlockedPushEntrySList(&ready_events, &event->entry);
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

    bool uninstallHooks(std::string &error)
    {
        if (!hooks_installed.load(std::memory_order_acquire)) {
            return true;
        }

        SuspendedProcessThreads suspended;
        if (!suspended.suspendStable(error)) {
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
            const std::uint64_t active_calls =
                active_hook_calls.load(std::memory_order_acquire);
            std::string resume_error;
            const bool resumed = suspended.resume(resume_error);
            if (!inspected) {
                error = "GetThreadContext failed for thread " + std::to_string(inspect_thread) +
                        ": " + std::to_string(inspect_failure);
                if (!resume_error.empty()) {
                    error += "; " + resume_error;
                }
                return false;
            }
            if (!resumed) {
                error = resume_error;
                return false;
            }
            if (!instruction_in_protected_code && active_calls == 0) {
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
        return true;
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
            module_id = modules.intern(module_path);
            module_cache.emplace(module_base, module_id);
        }

        FrameKey key;
        key.module = module_id;
        key.rva = module_base != 0 ? raw_address - module_base : raw_address;
        key.raw_address = raw_address;
        return key;
    }

    void acceptSample(const Sample &sample)
    {
        tree.log(sample.frames, sample.window, sample.weight);
        sample_count.fetch_add(1, std::memory_order_relaxed);
        sampled_bytes.fetch_add(sample.weight, std::memory_order_relaxed);
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
        buckets.erase(it);
    }

    bool buildSample(void *const *frames, std::uint16_t depth, std::uint64_t tick_id,
                     std::int32_t window, std::uint64_t weight, Sample &sample)
    {
        sample.tick_id = tick_id;
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
        Sample sample;
        if (!buildSample(event->frames, event->depth, event->tick_id, event->window,
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
            buckets[sample.tick_id].push_back(std::move(sample));
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
            const std::uint64_t age = stopped_ms >= allocation.allocated_ms
                                          ? stopped_ms - allocation.allocated_ms
                                          : 0;
            total_age += age;
            maximum_age = (std::max)(maximum_age, age);
            if (!tickAccepts(allocation.tick_id)) {
                continue;
            }
            Sample sample;
            if (buildSample(allocation.frames, allocation.depth, allocation.tick_id,
                            allocation.window, allocation.weight_bytes, sample)) {
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
        while (ticks.try_dequeue(tick)) {
            const bool keep = !ticked || tick.mspt_ms > threshold;
            if (ticked) {
                if (tick_decisions.size() <= tick.tick_id) {
                    tick_decisions.resize(static_cast<std::size_t>(tick.tick_id + 1), 0);
                }
                tick_decisions[static_cast<std::size_t>(tick.tick_id)] = keep ? 2 : 1;
            }
            flushOrDrop(tick.tick_id, keep);
        }

        PSLIST_ENTRY list = ::InterlockedFlushSList(&ready_events);
        while (list != nullptr) {
            PSLIST_ENTRY next = list->Next;
            auto *event = CONTAINING_RECORD(list, AllocationEvent, entry);
            processEvent(event);
            recycleEvent(event);
            list = next;
        }
    }

    void aggregatorLoop()
    {
        TrackingSuppressionGuard suppress;
        if (config.fail_aggregator_for_testing) {
            throw std::runtime_error("injected allocation aggregator failure");
        }
        while (aggregator_running.load(std::memory_order_acquire)) {
            drainQueues();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        drainQueues();
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
        while (ticks.try_dequeue(tick)) {
        }
        tree = CallTree{};
        modules = ModuleTable{};
        window_ticks.clear();
        buckets.clear();
        tick_decisions.clear();
        module_cache.clear();
        current_tick.store(0, std::memory_order_relaxed);
        sample_count.store(0, std::memory_order_relaxed);
        sampled_bytes.store(0, std::memory_order_relaxed);
        observed_bytes.store(0, std::memory_order_relaxed);
        dropped_samples.store(0, std::memory_order_relaxed);
        tracking_hook_calls.store(0, std::memory_order_relaxed);
        next_allocation_id.store(1, std::memory_order_relaxed);
        freed_samples.store(0, std::memory_order_relaxed);
        freed_bytes.store(0, std::memory_order_relaxed);
        live_samples.store(0, std::memory_order_relaxed);
        live_bytes.store(0, std::memory_order_relaxed);
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
        if (new_config.target_tid == 0) {
            error = "the target server thread is not available";
            return false;
        }
        if (new_config.interval_bytes <= 0) {
            error = "allocation sampling interval must be greater than zero";
            return false;
        }

        resetSession();
        config = new_config;
        target_tid.store(new_config.target_tid, std::memory_order_relaxed);
        interval_bytes.store(static_cast<std::uint64_t>(new_config.interval_bytes), std::memory_order_relaxed);
        started_tick_ms.store(monotonicMs(), std::memory_order_relaxed);
        const std::uint64_t new_generation = generation.fetch_add(1, std::memory_order_relaxed) + 1;
        sampling_seed.store(new_generation ^ monotonicMs() ^ new_config.target_tid,
                            std::memory_order_relaxed);

        if (!prepareHooks(error) || !allocateEventPool(error)) {
            return false;
        }

        if (!installHooks(error)) {
            freeEventPool();
            return false;
        }

        aggregator_running.store(true, std::memory_order_release);
        try {
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
            aggregator_running.store(false, std::memory_order_release);
            freeEventPool();
            error = "could not create the allocation aggregator thread";
            return false;
        }
        running.store(true, std::memory_order_release);
        tracking.store(true, std::memory_order_release);
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
            for (int attempt = 0;
                 attempt < 100 && hooks_installed.load(std::memory_order_acquire); ++attempt) {
                if (!uninstallHooks(last_error)) {
                    ::Sleep(10);
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
        TrackingSuppressionGuard suppress;
        const std::uint64_t finished = current_tick.fetch_add(1, std::memory_order_relaxed);
        ticks.enqueue(TickEvent{finished, mspt_ms});

        const std::uint64_t started = started_tick_ms.load(std::memory_order_relaxed);
        const std::uint64_t now = monotonicMs();
        const std::int32_t window =
            static_cast<std::int32_t>(started > 0 && now >= started ? (now - started) / 1000 : 0);
        WindowTickStats &stats = window_ticks[window];
        stats.ticks += 1;
        stats.mspt_sum += mspt_ms;
        stats.mspt_max = (std::max)(stats.mspt_max, mspt_ms);
    }
};

std::atomic<AllocationSampler::Impl *> AllocationSampler::Impl::active_instance{nullptr};
std::atomic<std::uint64_t> AllocationSampler::Impl::active_hook_calls{0};
thread_local bool AllocationSampler::Impl::inside_hook = false;
thread_local bool AllocationSampler::Impl::tracking_suppressed = false;
thread_local ByteSamplingState AllocationSampler::Impl::thread_sampling{};

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

std::uint64_t AllocationSampler::sampleCount() const
{
    return impl_->sample_count.load(std::memory_order_relaxed);
}

std::uint64_t AllocationSampler::sampledBytes() const
{
    return impl_->sampled_bytes.load(std::memory_order_relaxed);
}

std::uint64_t AllocationSampler::observedBytes() const
{
    return impl_->observed_bytes.load(std::memory_order_relaxed);
}

std::uint64_t AllocationSampler::droppedSamples() const
{
    return impl_->dropped_samples.load(std::memory_order_relaxed);
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
    const std::uint64_t count = impl_->live_samples.load(std::memory_order_relaxed);
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

}  // namespace spark
