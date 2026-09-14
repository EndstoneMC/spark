#include <type_traits>

#include "native/alloc/windows_permanent_iat_gateway.h"

namespace spark::test {
namespace {

constexpr DWORD kEntryError = 0x13572468;
constexpr DWORD kAllocatorError = 0x24681357;
alignas(64) std::array<unsigned char, 256> storage{};

struct Observation {
    DWORD entry_error = 0;
    std::size_t calls = 0;
    std::size_t family = 0;
    std::array<std::uintptr_t, 5> arguments{};
    bool operator==(const Observation &) const = default;
};

Observation observed;
bool preserve_error = false;
bool succeed = false;
std::size_t checked_cases = 0;

constexpr DWORD kSehEntryError = 0x31415926;
constexpr DWORD kSehAllocatorError = 0x27182818;
constexpr DWORD kSehExceptionCode = 0xE0420101;
alignas(64) std::array<unsigned char, 256> seh_storage{};

extern "C" void *WINAPI throwingHeapAllocForTesting(HANDLE, DWORD, SIZE_T) noexcept
{
    ::SetLastError(kSehAllocatorError);
    ::RaiseException(kSehExceptionCode, 0, 0, nullptr);
    return nullptr;
}

extern "C" void *WINAPI throwingHeapReAllocForTesting(HANDLE, DWORD, void *, SIZE_T) noexcept
{
    ::SetLastError(kSehAllocatorError);
    ::RaiseException(kSehExceptionCode, 0, 0, nullptr);
    return nullptr;
}

template <typename Function, typename... Args>
bool invokeSeh(Function function, DWORD &code, Args... args) noexcept
{
    code = 0;
    __try {
        (void)function(args...);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        code = ::GetExceptionCode();
        return true;
    }
    return false;
}

template <typename T>
std::uintptr_t argumentWord(T value)
{
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<std::uintptr_t>(value);
    }
    else {
        return static_cast<std::uintptr_t>(value);
    }
}

template <std::size_t Family, typename Result, typename... Args>
Result __cdecl allocatorStub(Args... args)
{
    const DWORD entry = ::GetLastError();
    observed.entry_error = entry;
    ++observed.calls;
    observed.family = Family;
    observed.arguments = {argumentWord(args)...};
    if (!preserve_error) {
        ::SetLastError(kAllocatorError);
    }
    if constexpr (std::is_pointer_v<Result>) {
        return succeed ? storage.data() + 128 : nullptr;
    }
    else if constexpr (!std::is_void_v<Result>) {
        return succeed ? TRUE : FALSE;
    }
}

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
std::uintptr_t invoke(Function function)
{
    if constexpr (std::is_void_v<decltype(function())>) {
        function();
        return 0;
    }
    else {
        return argumentWord(function());
    }
}

template <typename Direct, typename Hooked>
void compareCalls(const char *name, Direct direct, Hooked hooked)
{
    observed = {};
    ::SetLastError(kEntryError);
    const auto expected_result = invoke(direct);
    const DWORD expected_error = ::GetLastError();
    const Observation expected = observed;
    observed = {};
    ::SetLastError(kEntryError);
    const auto actual_result = invoke(hooked);
    const DWORD actual_error = ::GetLastError();
    if (observed != expected || actual_result != expected_result || actual_error != expected_error ||
        observed.calls != 1 || observed.entry_error != kEntryError) {
        std::fprintf(stderr, "%s: entry=%lu expected=%lu final=%lu expected=%lu calls=%zu\n", name,
                     observed.entry_error, expected.entry_error, actual_error, expected_error, observed.calls);
        throw std::runtime_error("allocator handler differs from direct call");
    }
    ++checked_cases;
}

enum class Mode {
    FreshTls,
    CachedTls,
    Suppressed,
    Reentry,
    TrackingOff,
    CountOnly,
    CountReentry,
    NoTls,
    Tombstone
};

template <typename Impl>
void prepareMode(Impl &impl, Mode mode)
{
    impl.releaseThreadStateRegistry();
    impl.config.count_only = mode == Mode::CountOnly || mode == Mode::CountReentry;
    impl.tracking.store(mode != Mode::TrackingOff);
    Impl::mCountOnlyInsideHook = mode == Mode::CountReentry;
    if (mode == Mode::NoTls) {
        return;
    }
    impl.tls_index = ::TlsAlloc();
    require(impl.tls_index != TLS_OUT_OF_INDEXES, "TlsAlloc failed");
    if (mode == Mode::Tombstone) {
        require(::TlsSetValue(impl.tls_index, reinterpret_cast<void *>(std::uintptr_t{1})) != FALSE,
                "TlsSetValue failed");
    }
    else if (mode == Mode::CachedTls || mode == Mode::Suppressed || mode == Mode::Reentry) {
        auto *state = impl.currentThreadState();
        require(state != nullptr && state->registry_state.load() == 2 && state->thread_handle != nullptr,
                "production TLS registration failed");
        state->tracking_suppressed = mode == Mode::Suppressed;
        state->inside_hook = mode == Mode::Reentry;
    }
}

template <typename Impl>
void verifyMode(Impl &impl, Mode mode)
{
    if (mode == Mode::FreshTls || mode == Mode::CachedTls || mode == Mode::Suppressed || mode == Mode::Reentry) {
        auto *state = impl.currentThreadState();
        require(state != nullptr && state->registry_state.load() == 2 && state->thread_handle != nullptr,
                "production TLS path was bypassed");
        require(state->inside_hook == (mode == Mode::Reentry), "recursion guard ownership changed");
    }
    require(Impl::mCountOnlyInsideHook == (mode == Mode::CountReentry), "count-only recursion ownership changed");
    require(impl.hotCountersForCurrentThread().tracking_hook_calls.load() == 0, "tracking guard did not release");
}

class BadAllocMarker final : public std::bad_alloc {
public:
    [[nodiscard]] const char *what() const noexcept override { return "spark allocation new-handler marker"; }
};

constexpr DWORD kBadAllocError = 0x44556677;
alignas(64) std::array<unsigned char, 512> bad_alloc_storage{};

template <std::size_t Family, typename Result, typename... Args>
[[noreturn]] Result __cdecl badAllocStub(Args...)
{
    (void)Family;
    ::SetLastError(kBadAllocError);
    throw BadAllocMarker{};
}

template <typename Impl, typename DirectCall, typename GatewayCall>
void runBadAllocPair(Impl &impl, const char *name, void *original, void *handler, std::uint32_t stack_arguments,
                     DirectCall direct_call, GatewayCall gateway_call, void *record_pointer = nullptr,
                     Mode mode = Mode::CachedTls, bool preowned = false)
{
    using namespace spark::permanent_iat_gateway;
    prepareMode(impl, mode);
    typename Impl::ThreadSamplingState *preowned_state = nullptr;
    if (preowned) {
        preowned_state = impl.currentThreadState();
        require(preowned_state != nullptr, "bad_alloc pre-owned recursion state missing");
        preowned_state->inside_hook = true;
    }
    impl.interval_bytes.store(1, std::memory_order_relaxed);
    const std::uint64_t live_before = impl.live_samples.load(std::memory_order_acquire);
    if (record_pointer != nullptr) {
        impl.recordAllocation(record_pointer, 1);
        require(impl.live_samples.load(std::memory_order_acquire) == live_before + 1,
                "bad_alloc realloc setup record missing");
    }

    ::SetLastError(kEntryError);
    bool direct_caught = false;
    try {
        (void)direct_call();
    }
    catch (const BadAllocMarker &exception) {
        direct_caught = std::strcmp(exception.what(), "spark allocation new-handler marker") == 0;
    }
    catch (...) {
    }
    require(direct_caught, name);
    require(::GetLastError() == kBadAllocError, "bad_alloc direct LastError changed");
    require(impl.hotCountersForCurrentThread().tracking_hook_calls.load(std::memory_order_acquire) == 0,
            "bad_alloc direct tracking counter leaked");
    if (preowned) {
        require(preowned_state->inside_hook, "bad_alloc direct pre-owned recursion was cleared");
    }
    else if (mode == Mode::CountOnly) {
        require(!Impl::mCountOnlyInsideHook, "bad_alloc direct count-only recursion leaked");
    }
    else if (mode != Mode::TrackingOff) {
        require(!impl.currentThreadState()->inside_hook, "bad_alloc direct recursion ownership leaked");
    }
    for (const auto &counter : Impl::mActiveHookCalls) {
        require(counter.value.load(std::memory_order_acquire) == 0, "bad_alloc direct hook counter leaked");
    }

    PermanentIatGatewayHandle gateway;
    std::string error;
    require(createPermanentIatGateway(original, stack_arguments, gateway, error), "bad_alloc gateway creation failed");
    require(bindPermanentIatGateway(gateway, handler, 5000, error), "bad_alloc gateway bind failed");
    ::SetLastError(kEntryError);
    bool gateway_caught = false;
    try {
        (void)gateway_call(gateway.gateway);
    }
    catch (const BadAllocMarker &exception) {
        gateway_caught = std::strcmp(exception.what(), "spark allocation new-handler marker") == 0;
    }
    catch (...) {
    }
    require(gateway_caught, name);
    require(::GetLastError() == kBadAllocError, "bad_alloc gateway LastError changed");
    require(permanentIatGatewayActive(gateway) == 0, "bad_alloc gateway active leaked");
    require(impl.hotCountersForCurrentThread().tracking_hook_calls.load(std::memory_order_acquire) == 0,
            "bad_alloc gateway tracking counter leaked");
    if (preowned) {
        require(preowned_state->inside_hook, "bad_alloc gateway pre-owned recursion was cleared");
        preowned_state->inside_hook = false;
    }
    else if (mode == Mode::CountOnly) {
        require(!Impl::mCountOnlyInsideHook, "bad_alloc gateway count-only recursion leaked");
    }
    else if (mode != Mode::TrackingOff) {
        require(!impl.currentThreadState()->inside_hook, "bad_alloc gateway recursion ownership leaked");
    }
    for (const auto &counter : Impl::mActiveHookCalls) {
        require(counter.value.load(std::memory_order_acquire) == 0, "bad_alloc gateway hook counter leaked");
    }
    require(detachPermanentIatGateway(gateway, 5000, error), "bad_alloc gateway detach failed");

    if (record_pointer != nullptr) {
        require(impl.live_samples.load(std::memory_order_acquire) == live_before + 1,
                "bad_alloc realloc record was lost");
        auto *restored = impl.detachAllocation(record_pointer);
        require(restored != nullptr, "bad_alloc realloc restored record missing");
        impl.retireAllocation(restored, 0);
    }
}

template <typename Impl>
void runMatrix(Impl &impl)
{
    impl.real_malloc = &allocatorStub<0, void *, std::size_t>;
    impl.real_calloc = &allocatorStub<1, void *, std::size_t, std::size_t>;
    impl.real_realloc = &allocatorStub<2, void *, void *, std::size_t>;
    impl.real_recalloc = &allocatorStub<3, void *, void *, std::size_t, std::size_t>;
    impl.real_free = &allocatorStub<4, void, void *>;
    impl.real_aligned_malloc = &allocatorStub<5, void *, std::size_t, std::size_t>;
    impl.real_aligned_realloc = &allocatorStub<6, void *, void *, std::size_t, std::size_t>;
    impl.real_aligned_recalloc = &allocatorStub<7, void *, void *, std::size_t, std::size_t, std::size_t>;
    impl.real_aligned_offset_malloc = &allocatorStub<8, void *, std::size_t, std::size_t, std::size_t>;
    impl.real_aligned_offset_realloc = &allocatorStub<9, void *, void *, std::size_t, std::size_t, std::size_t>;
    impl.real_aligned_offset_recalloc =
        &allocatorStub<10, void *, void *, std::size_t, std::size_t, std::size_t, std::size_t>;
    impl.real_aligned_free = &allocatorStub<11, void, void *>;
    impl.real_malloc_base = &allocatorStub<12, void *, std::size_t>;
    impl.real_calloc_base = &allocatorStub<13, void *, std::size_t, std::size_t>;
    impl.real_realloc_base = &allocatorStub<14, void *, void *, std::size_t>;
    impl.real_free_base = &allocatorStub<15, void, void *>;
    impl.real_heap_alloc = &allocatorStub<16, void *, HANDLE, DWORD, SIZE_T>;
    impl.real_heap_realloc = &allocatorStub<17, void *, HANDLE, DWORD, void *, SIZE_T>;
    impl.real_heap_free = &allocatorStub<18, BOOL, HANDLE, DWORD, void *>;
    void *pointer = storage.data();
    HANDLE heap = storage.data() + 64;
    for (const auto mode : {Mode::FreshTls, Mode::CachedTls, Mode::Suppressed, Mode::Reentry, Mode::TrackingOff,
                            Mode::CountOnly, Mode::CountReentry, Mode::NoTls, Mode::Tombstone}) {
        for (const bool preserve : {false, true}) {
            preserve_error = preserve;
            for (const bool success : {false, true}) {
                succeed = success;
#define CHECK_ALLOCATOR(member, hook, ...)                                                                  \
    prepareMode(impl, mode);                                                                                \
    compareCalls(#hook, [&] { return impl.member(__VA_ARGS__); }, [&] { return Impl::hook(__VA_ARGS__); }); \
    verifyMode(impl, mode)
                CHECK_ALLOCATOR(real_malloc, hookMalloc, 17);
                CHECK_ALLOCATOR(real_calloc, hookCalloc, 3, 17);
                CHECK_ALLOCATOR(real_realloc, hookRealloc, pointer, 17);
                CHECK_ALLOCATOR(real_recalloc, hookRecalloc, pointer, 3, 17);
                CHECK_ALLOCATOR(real_free, hookFree, pointer);
                CHECK_ALLOCATOR(real_aligned_malloc, hookAlignedMalloc, 17, 64);
                CHECK_ALLOCATOR(real_aligned_realloc, hookAlignedRealloc, pointer, 17, 64);
                CHECK_ALLOCATOR(real_aligned_recalloc, hookAlignedRecalloc, pointer, 3, 17, 64);
                CHECK_ALLOCATOR(real_aligned_offset_malloc, hookAlignedOffsetMalloc, 17, 64, 5);
                CHECK_ALLOCATOR(real_aligned_offset_realloc, hookAlignedOffsetRealloc, pointer, 17, 64, 5);
                CHECK_ALLOCATOR(real_aligned_offset_recalloc, hookAlignedOffsetRecalloc, pointer, 3, 17, 64, 5);
                CHECK_ALLOCATOR(real_aligned_free, hookAlignedFree, pointer);
                CHECK_ALLOCATOR(real_malloc_base, hookMallocBase, 17);
                CHECK_ALLOCATOR(real_calloc_base, hookCallocBase, 3, 17);
                CHECK_ALLOCATOR(real_realloc_base, hookReallocBase, pointer, 17);
                CHECK_ALLOCATOR(real_free_base, hookFreeBase, pointer);
                CHECK_ALLOCATOR(real_heap_alloc, hookHeapAlloc, heap, 0x40, 17);
                CHECK_ALLOCATOR(real_heap_realloc, hookHeapReAlloc, heap, 0x40, pointer, 17);
                CHECK_ALLOCATOR(real_heap_free, hookHeapFree, heap, 0x40, pointer);
#undef CHECK_ALLOCATOR
            }
        }
    }
}

template <typename Impl>
void runHeapExceptionMatrix(Impl &impl)
{
    using namespace spark::permanent_iat_gateway;
    prepareMode(impl, Mode::CachedTls);
    impl.real_heap_alloc = &throwingHeapAllocForTesting;
    PermanentIatGatewayHandle alloc_gateway;
    std::string error;
    require(createPermanentIatGateway(reinterpret_cast<void *>(&throwingHeapAllocForTesting), 0, alloc_gateway, error),
            "SEH HeapAlloc gateway creation failed");
    require(bindPermanentIatGateway(alloc_gateway, reinterpret_cast<void *>(&Impl::hookHeapAlloc), 5000, error),
            "SEH HeapAlloc gateway bind failed");

    ::SetLastError(kSehEntryError);
    DWORD exception_code = 0;
    require(invokeSeh(&Impl::hookHeapAlloc, exception_code, static_cast<HANDLE>(seh_storage.data()), 0,
                      static_cast<SIZE_T>(64)),
            "direct SEH HeapAlloc did not propagate");
    require(exception_code == kSehExceptionCode, "direct SEH HeapAlloc code changed");
    require(::GetLastError() == kSehAllocatorError, "direct SEH HeapAlloc LastError changed");
    require(invokeSeh(reinterpret_cast<typename Impl::HeapAllocFn>(alloc_gateway.gateway), exception_code,
                      static_cast<HANDLE>(seh_storage.data()), 0, static_cast<SIZE_T>(64)),
            "SEH HeapAlloc did not propagate");
    require(exception_code == kSehExceptionCode, "SEH HeapAlloc code changed");
    require(::GetLastError() == kSehAllocatorError, "SEH HeapAlloc LastError changed");
    require(permanentIatGatewayActive(alloc_gateway) == 0, "SEH HeapAlloc gateway active leaked");
    require(impl.hotCountersForCurrentThread().tracking_hook_calls.load(std::memory_order_acquire) == 0,
            "SEH HeapAlloc tracking counter leaked");
    require(!impl.currentThreadState()->inside_hook, "SEH HeapAlloc recursion ownership leaked");
    for (const auto &counter : Impl::mActiveHookCalls) {
        require(counter.value.load(std::memory_order_acquire) == 0, "SEH HeapAlloc hook counter leaked");
    }
    require(detachPermanentIatGateway(alloc_gateway, 5000, error), "SEH HeapAlloc gateway detach failed");

    prepareMode(impl, Mode::CachedTls);
    impl.real_heap_realloc = &throwingHeapReAllocForTesting;
    impl.interval_bytes.store(1, std::memory_order_relaxed);
    const std::uint64_t live_before_setup = impl.live_samples.load(std::memory_order_acquire);
    impl.recordAllocation(seh_storage.data(), 64);
    require(impl.live_samples.load(std::memory_order_acquire) == live_before_setup + 1,
            "SEH HeapReAlloc setup record missing");
    PermanentIatGatewayHandle realloc_gateway;
    require(
        createPermanentIatGateway(reinterpret_cast<void *>(&throwingHeapReAllocForTesting), 0, realloc_gateway, error),
        "SEH HeapReAlloc gateway creation failed");
    require(bindPermanentIatGateway(realloc_gateway, reinterpret_cast<void *>(&Impl::hookHeapReAlloc), 5000, error),
            "SEH HeapReAlloc gateway bind failed");

    ::SetLastError(kSehEntryError);
    exception_code = 0;
    require(invokeSeh(reinterpret_cast<typename Impl::HeapReAllocFn>(realloc_gateway.gateway), exception_code,
                      static_cast<HANDLE>(seh_storage.data()), 0, seh_storage.data(), static_cast<SIZE_T>(128)),
            "SEH HeapReAlloc did not propagate");
    require(exception_code == kSehExceptionCode, "SEH HeapReAlloc code changed");
    require(::GetLastError() == kSehAllocatorError, "SEH HeapReAlloc LastError changed");
    require(permanentIatGatewayActive(realloc_gateway) == 0, "SEH HeapReAlloc gateway active leaked");
    require(impl.live_samples.load(std::memory_order_acquire) == live_before_setup + 1,
            "SEH HeapReAlloc detached record was lost");
    require(impl.hotCountersForCurrentThread().tracking_hook_calls.load(std::memory_order_acquire) == 0,
            "SEH HeapReAlloc tracking counter leaked");
    require(!impl.currentThreadState()->inside_hook, "SEH HeapReAlloc recursion ownership leaked");
    require(detachPermanentIatGateway(realloc_gateway, 5000, error), "SEH HeapReAlloc gateway detach failed");
    auto *restored = impl.detachAllocation(seh_storage.data());
    require(restored != nullptr, "SEH HeapReAlloc restored record missing");
    impl.retireAllocation(restored, 0);
}

template <typename Impl>
void runHeapExceptionOwnershipModes(Impl &impl)
{
    using namespace spark::permanent_iat_gateway;
    const auto run = [&](Mode mode, bool preowned) {
        prepareMode(impl, mode);
        impl.real_heap_alloc = &throwingHeapAllocForTesting;
        typename Impl::ThreadSamplingState *state = nullptr;
        if (preowned) {
            state = impl.currentThreadState();
            require(state != nullptr, "preowned SEH state missing");
            state->inside_hook = true;
        }

        PermanentIatGatewayHandle gateway;
        std::string error;
        require(createPermanentIatGateway(reinterpret_cast<void *>(&throwingHeapAllocForTesting), 0, gateway, error),
                "ownership-mode gateway creation failed");
        require(bindPermanentIatGateway(gateway, reinterpret_cast<void *>(&Impl::hookHeapAlloc), 5000, error),
                "ownership-mode gateway bind failed");
        DWORD exception_code = 0;
        require(invokeSeh(reinterpret_cast<typename Impl::HeapAllocFn>(gateway.gateway), exception_code,
                          static_cast<HANDLE>(seh_storage.data()), 0, static_cast<SIZE_T>(64)),
                "ownership-mode SEH did not propagate");
        require(exception_code == kSehExceptionCode, "ownership-mode SEH code changed");
        require(permanentIatGatewayActive(gateway) == 0, "ownership-mode gateway active leaked");
        require(impl.hotCountersForCurrentThread().tracking_hook_calls.load(std::memory_order_acquire) == 0,
                "ownership-mode tracking counter leaked");
        if (mode == Mode::CountOnly) {
            require(!Impl::mCountOnlyInsideHook, "count-only recursion ownership leaked");
        }
        if (preowned) {
            require(state->inside_hook, "preowned recursion ownership was cleared");
            state->inside_hook = false;
        }
        require(detachPermanentIatGateway(gateway, 5000, error), "ownership-mode gateway detach failed");
    };

    run(Mode::TrackingOff, false);
    run(Mode::CountOnly, false);
    run(Mode::CachedTls, true);
}

template <typename Impl>
void runBadAllocMatrix(Impl &impl)
{
    using MallocFn = typename Impl::MallocFn;
    using CallocFn = typename Impl::CallocFn;
    using ReallocFn = typename Impl::ReallocFn;
    using RecallocFn = typename Impl::RecallocFn;
    using AlignedMallocFn = typename Impl::AlignedMallocFn;
    using AlignedReallocFn = typename Impl::AlignedReallocFn;
    using AlignedRecallocFn = typename Impl::AlignedRecallocFn;
    using AlignedOffsetMallocFn = typename Impl::AlignedOffsetMallocFn;
    using AlignedOffsetReallocFn = typename Impl::AlignedOffsetReallocFn;
    using AlignedOffsetRecallocFn = typename Impl::AlignedOffsetRecallocFn;

    impl.real_malloc = &badAllocStub<0, void *, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc malloc", reinterpret_cast<void *>(impl.real_malloc),
        reinterpret_cast<void *>(&Impl::hookMalloc), 0, [&] { return Impl::hookMalloc(17); },
        [](void *gateway) { return reinterpret_cast<MallocFn>(gateway)(17); });

    impl.real_calloc = &badAllocStub<1, void *, std::size_t, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc calloc", reinterpret_cast<void *>(impl.real_calloc),
        reinterpret_cast<void *>(&Impl::hookCalloc), 0, [&] { return Impl::hookCalloc(3, 17); },
        [](void *gateway) { return reinterpret_cast<CallocFn>(gateway)(3, 17); });

    impl.real_realloc = &badAllocStub<2, void *, void *, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc realloc", reinterpret_cast<void *>(impl.real_realloc),
        reinterpret_cast<void *>(&Impl::hookRealloc), 0,
        [&] { return Impl::hookRealloc(bad_alloc_storage.data(), 17); },
        [](void *gateway) { return reinterpret_cast<ReallocFn>(gateway)(bad_alloc_storage.data(), 17); },
        bad_alloc_storage.data());

    impl.real_recalloc = &badAllocStub<3, void *, void *, std::size_t, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc recalloc", reinterpret_cast<void *>(impl.real_recalloc),
        reinterpret_cast<void *>(&Impl::hookRecalloc), 0,
        [&] { return Impl::hookRecalloc(bad_alloc_storage.data() + 32, 3, 17); },
        [](void *gateway) { return reinterpret_cast<RecallocFn>(gateway)(bad_alloc_storage.data() + 32, 3, 17); },
        bad_alloc_storage.data() + 32);

    impl.real_aligned_malloc = &badAllocStub<4, void *, std::size_t, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc aligned_malloc", reinterpret_cast<void *>(impl.real_aligned_malloc),
        reinterpret_cast<void *>(&Impl::hookAlignedMalloc), 0, [&] { return Impl::hookAlignedMalloc(17, 64); },
        [](void *gateway) { return reinterpret_cast<AlignedMallocFn>(gateway)(17, 64); });

    impl.real_aligned_realloc = &badAllocStub<5, void *, void *, std::size_t, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc aligned_realloc", reinterpret_cast<void *>(impl.real_aligned_realloc),
        reinterpret_cast<void *>(&Impl::hookAlignedRealloc), 0,
        [&] { return Impl::hookAlignedRealloc(bad_alloc_storage.data() + 64, 17, 64); },
        [](void *gateway) {
            return reinterpret_cast<AlignedReallocFn>(gateway)(bad_alloc_storage.data() + 64, 17, 64);
        },
        bad_alloc_storage.data() + 64);

    impl.real_aligned_recalloc = &badAllocStub<6, void *, void *, std::size_t, std::size_t, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc aligned_recalloc", reinterpret_cast<void *>(impl.real_aligned_recalloc),
        reinterpret_cast<void *>(&Impl::hookAlignedRecalloc), 0,
        [&] { return Impl::hookAlignedRecalloc(bad_alloc_storage.data() + 96, 3, 17, 64); },
        [](void *gateway) {
            return reinterpret_cast<AlignedRecallocFn>(gateway)(bad_alloc_storage.data() + 96, 3, 17, 64);
        },
        bad_alloc_storage.data() + 96);

    impl.real_aligned_offset_malloc = &badAllocStub<7, void *, std::size_t, std::size_t, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc aligned_offset_malloc", reinterpret_cast<void *>(impl.real_aligned_offset_malloc),
        reinterpret_cast<void *>(&Impl::hookAlignedOffsetMalloc), 0,
        [&] { return Impl::hookAlignedOffsetMalloc(17, 64, 5); },
        [](void *gateway) { return reinterpret_cast<AlignedOffsetMallocFn>(gateway)(17, 64, 5); });

    impl.real_aligned_offset_realloc = &badAllocStub<8, void *, void *, std::size_t, std::size_t, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc aligned_offset_realloc", reinterpret_cast<void *>(impl.real_aligned_offset_realloc),
        reinterpret_cast<void *>(&Impl::hookAlignedOffsetRealloc), 0,
        [&] { return Impl::hookAlignedOffsetRealloc(bad_alloc_storage.data() + 160, 17, 64, 5); },
        [](void *gateway) {
            return reinterpret_cast<AlignedOffsetReallocFn>(gateway)(bad_alloc_storage.data() + 160, 17, 64, 5);
        },
        bad_alloc_storage.data() + 160);

    impl.real_aligned_offset_recalloc =
        &badAllocStub<9, void *, void *, std::size_t, std::size_t, std::size_t, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc aligned_offset_recalloc", reinterpret_cast<void *>(impl.real_aligned_offset_recalloc),
        reinterpret_cast<void *>(&Impl::hookAlignedOffsetRecalloc), 1,
        [&] { return Impl::hookAlignedOffsetRecalloc(bad_alloc_storage.data() + 192, 3, 17, 64, 5); },
        [](void *gateway) {
            return reinterpret_cast<AlignedOffsetRecallocFn>(gateway)(bad_alloc_storage.data() + 192, 3, 17, 64, 5);
        },
        bad_alloc_storage.data() + 192);

    impl.real_malloc_base = &badAllocStub<10, void *, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc malloc_base", reinterpret_cast<void *>(impl.real_malloc_base),
        reinterpret_cast<void *>(&Impl::hookMallocBase), 0, [&] { return Impl::hookMallocBase(17); },
        [](void *gateway) { return reinterpret_cast<MallocFn>(gateway)(17); });

    impl.real_calloc_base = &badAllocStub<11, void *, std::size_t, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc calloc_base", reinterpret_cast<void *>(impl.real_calloc_base),
        reinterpret_cast<void *>(&Impl::hookCallocBase), 0, [&] { return Impl::hookCallocBase(3, 17); },
        [](void *gateway) { return reinterpret_cast<CallocFn>(gateway)(3, 17); });

    impl.real_realloc_base = &badAllocStub<12, void *, void *, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc realloc_base", reinterpret_cast<void *>(impl.real_realloc_base),
        reinterpret_cast<void *>(&Impl::hookReallocBase), 0,
        [&] { return Impl::hookReallocBase(bad_alloc_storage.data() + 224, 17); },
        [](void *gateway) { return reinterpret_cast<ReallocFn>(gateway)(bad_alloc_storage.data() + 224, 17); },
        bad_alloc_storage.data() + 224);

    impl.real_malloc = &badAllocStub<0, void *, std::size_t>;
    runBadAllocPair(
        impl, "bad_alloc tracking-off", reinterpret_cast<void *>(impl.real_malloc),
        reinterpret_cast<void *>(&Impl::hookMalloc), 0, [&] { return Impl::hookMalloc(17); },
        [](void *gateway) { return reinterpret_cast<MallocFn>(gateway)(17); }, nullptr, Mode::TrackingOff);
    runBadAllocPair(
        impl, "bad_alloc count-only", reinterpret_cast<void *>(impl.real_malloc),
        reinterpret_cast<void *>(&Impl::hookMalloc), 0, [&] { return Impl::hookMalloc(17); },
        [](void *gateway) { return reinterpret_cast<MallocFn>(gateway)(17); }, nullptr, Mode::CountOnly);
    runBadAllocPair(
        impl, "bad_alloc pre-owned-recursion", reinterpret_cast<void *>(impl.real_malloc),
        reinterpret_cast<void *>(&Impl::hookMalloc), 0, [&] { return Impl::hookMalloc(17); },
        [](void *gateway) { return reinterpret_cast<MallocFn>(gateway)(17); }, nullptr, Mode::CachedTls, true);
}

}  // namespace

std::size_t windowsAllocationLastErrorCaseCount() noexcept
{
    return checked_cases;
}

template <typename Impl>
void runWindowsAllocationLastErrorTests(Impl &impl)
{
    std::string error;
    require(impl.allocateEventPool(error), "event pool allocation failed");
    Impl::mActiveInstance.store(&impl);
    try {
        runMatrix(impl);
        runHeapExceptionMatrix(impl);
        runHeapExceptionOwnershipModes(impl);
        runBadAllocMatrix(impl);
    }
    catch (...) {
        Impl::mActiveInstance.store(nullptr);
        Impl::mCountOnlyInsideHook = false;
        impl.releaseThreadStateRegistry();
        impl.freeEventPool();
        throw;
    }
    Impl::mActiveInstance.store(nullptr);
    Impl::mCountOnlyInsideHook = false;
    impl.tracking.store(false);
    impl.releaseThreadStateRegistry();
    impl.freeEventPool();
    impl.clearFunctionPointers();
    std::printf("Windows allocation LastError: %zu direct-call equivalence cases passed\n", checked_cases);
}

}  // namespace spark::test
