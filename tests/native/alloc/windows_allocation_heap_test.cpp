#include "native/alloc/allocation_diagnostics_test_access.h"
#include "native/alloc/allocation_sampler.h"

#ifndef _WIN32
#error "windows_allocation_heap_test.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using HeapAllocFn = void *(WINAPI *)(HANDLE, DWORD, SIZE_T);
using HeapReAllocFn = void *(WINAPI *)(HANDLE, DWORD, void *, SIZE_T);
using HeapFreeFn = BOOL(WINAPI *)(HANDLE, DWORD, void *);
using FixtureHeapAllocFn = void *(*)(HANDLE, DWORD, SIZE_T);
using FixtureHeapReAllocFn = void *(*)(HANDLE, DWORD, void *, SIZE_T);
using FixtureHeapFreeFn = BOOL (*)(HANDLE, DWORD, void *);

constexpr DWORD KEntryError = 0x13572468;
constexpr SIZE_T KSmallSize = 64;
constexpr SIZE_T KExpandedSize = 128;
constexpr SIZE_T KImpossibleSize = 1024 * 1024;
constexpr DWORD KCreateExceptionFlags = HEAP_GENERATE_EXCEPTIONS;
constexpr DWORD KCallExceptionFlags = HEAP_GENERATE_EXCEPTIONS;
constexpr DWORD KCallNormalFlags = 0;

[[nodiscard]] HeapAllocFn directHeapAlloc() noexcept
{
    return reinterpret_cast<HeapAllocFn>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "HeapAlloc"));
}

[[nodiscard]] HeapReAllocFn directHeapReAlloc() noexcept
{
    return reinterpret_cast<HeapReAllocFn>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "HeapReAlloc"));
}

[[nodiscard]] HeapFreeFn directHeapFree() noexcept
{
    return reinterpret_cast<HeapFreeFn>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "HeapFree"));
}

bool fail(const char *message)
{
    std::fprintf(stderr, "stage=windows-allocation-heap failure=%s\n", message);
    return false;
}

bool hasPattern(const void *pointer, SIZE_T size, unsigned char value)
{
    const auto *bytes = static_cast<const unsigned char *>(pointer);
    for (SIZE_T index = 0; index < size; ++index) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

bool activeCapability(const spark::AllocationSampler &sampler, const char *name)
{
    for (const auto &capability : sampler.hookCapabilities()) {
        if (capability.name == name) {
            return capability.status == spark::AllocationHookStatus::Active ||
                   capability.status == spark::AllocationHookStatus::Alias;
        }
    }
    return false;
}

bool validLiveRecord(const spark::test::AllocationLiveRecordState &state, SIZE_T expected_size) noexcept
{
    return state.found && state.allocation_id != 0 && state.requested_bytes == expected_size && state.weight_bytes != 0;
}

bool sameLiveRecord(const spark::test::AllocationLiveRecordState &before,
                    const spark::test::AllocationLiveRecordState &after) noexcept
{
    return after.found && after.allocation_id == before.allocation_id &&
           after.requested_bytes == before.requested_bytes && after.weight_bytes == before.weight_bytes;
}

template <typename Call>
bool invokeHeapException(Call call, DWORD &code) noexcept
{
    code = 0;
    __try {
        call();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        code = ::GetExceptionCode();
        return true;
    }
    return false;
}

}  // namespace

int main()
{
    auto const direct_alloc = directHeapAlloc();
    auto const direct_realloc = directHeapReAlloc();
    auto const direct_free = directHeapFree();
    if (direct_alloc == nullptr || direct_realloc == nullptr || direct_free == nullptr) {
        return fail("kernel32-heap-exports") ? 0 : 1;
    }

    HANDLE heap = ::HeapCreate(KCreateExceptionFlags, 0, 64 * 1024);
    if (heap == nullptr) {
        return fail("heap-create") ? 0 : 1;
    }

    DWORD direct_alloc_code = 0;
    void *direct_failed_alloc = nullptr;
    ::SetLastError(KEntryError);
    const bool direct_alloc_caught = invokeHeapException(
        [&] { direct_failed_alloc = direct_alloc(heap, KCallNormalFlags, KImpossibleSize); }, direct_alloc_code);
    const DWORD direct_alloc_error = ::GetLastError();
    if (!direct_alloc_caught || direct_failed_alloc != nullptr) {
        ::HeapDestroy(heap);
        return fail("direct-failed-allocation") ? 0 : 1;
    }

    void *direct_pointer = direct_alloc(heap, 0, KSmallSize);
    if (direct_pointer == nullptr) {
        ::HeapDestroy(heap);
        return fail("direct-small-allocation") ? 0 : 1;
    }
    std::memset(direct_pointer, 0xA5, KSmallSize);
    ::SetLastError(KEntryError);
    void *direct_failed = nullptr;
    DWORD direct_realloc_code = 0;
    const bool direct_realloc_caught = invokeHeapException(
        [&] { direct_failed = direct_realloc(heap, KCallNormalFlags, direct_pointer, KImpossibleSize); },
        direct_realloc_code);
    const DWORD direct_failure_error = ::GetLastError();
    if (!direct_realloc_caught || direct_failed != nullptr || !hasPattern(direct_pointer, KSmallSize, 0xA5)) {
        (void)direct_free(heap, 0, direct_failed != nullptr ? direct_failed : direct_pointer);
        ::HeapDestroy(heap);
        return fail("direct-failed-realloc") ? 0 : 1;
    }
    if (!direct_free(heap, 0, direct_pointer)) {
        ::HeapDestroy(heap);
        return fail("direct-failed-realloc-preserved") ? 0 : 1;
    }

    HMODULE fixture = ::LoadLibraryW(L".\\spark_windows_allocation_fixture.dll");
    if (fixture == nullptr) {
        ::HeapDestroy(heap);
        return fail("fixture-load") ? 0 : 1;
    }
    const auto fixture_alloc =
        reinterpret_cast<FixtureHeapAllocFn>(::GetProcAddress(fixture, "sparkAllocationFixtureHeapAlloc"));
    const auto fixture_realloc =
        reinterpret_cast<FixtureHeapReAllocFn>(::GetProcAddress(fixture, "sparkAllocationFixtureHeapReAlloc"));
    const auto fixture_free =
        reinterpret_cast<FixtureHeapFreeFn>(::GetProcAddress(fixture, "sparkAllocationFixtureHeapFree"));
    if (fixture_alloc == nullptr || fixture_realloc == nullptr || fixture_free == nullptr) {
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        return fail("fixture-heap-exports") ? 0 : 1;
    }

    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config;
    config.interval_bytes = 1;
    config.session_seed = 0x13579BDFULL;
    config.live_only = true;
    std::string error;
    if (!sampler.start(config, error)) {
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        std::fprintf(stderr, "stage=windows-allocation-heap failure=start detail=%s\n", error.c_str());
        return 1;
    }
    if (!activeCapability(sampler, "HeapAlloc") || !activeCapability(sampler, "HeapReAlloc") ||
        !activeCapability(sampler, "HeapFree")) {
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        return fail("heap-hook-capability") ? 0 : 1;
    }

    DWORD hooked_alloc_code = 0;
    void *hooked_failed_alloc = nullptr;
    ::SetLastError(KEntryError);
    const bool hooked_alloc_caught = invokeHeapException(
        [&] { hooked_failed_alloc = fixture_alloc(heap, KCallNormalFlags, KImpossibleSize); }, hooked_alloc_code);
    const DWORD hooked_alloc_error = ::GetLastError();
    if (!hooked_alloc_caught || hooked_failed_alloc != nullptr || hooked_alloc_code != direct_alloc_code ||
        hooked_alloc_error != direct_alloc_error) {
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        return fail("hooked-failed-allocation") ? 0 : 1;
    }

    const std::uint64_t hooks_before = sampler.hookCalls();
    const std::uint64_t successful_before = sampler.successfulAllocationCalls();
    const std::uint64_t live_before = sampler.liveSamples();
    ::SetLastError(KEntryError);
    void *pointer = fixture_alloc(heap, 0, KSmallSize);
    const DWORD allocation_error = ::GetLastError();
    spark::test::AllocationLiveRecordState allocation_state;
    const bool allocation_lookup = pointer != nullptr && spark::test::AllocationDiagnosticsTestAccess::liveRecordState(
                                                             sampler, pointer, allocation_state);
    const bool allocation_record = allocation_lookup && validLiveRecord(allocation_state, KSmallSize);
    if (pointer == nullptr || allocation_error != KEntryError || sampler.hookCalls() <= hooks_before ||
        sampler.successfulAllocationCalls() <= successful_before || !allocation_record) {
        std::fprintf(
            stderr,
            "stage=windows-allocation-heap detail=small pointer=%p error=%lu hooks=%llu/%llu successful=%llu/%llu "
            "live=%llu/%llu lookup=%d found=%d id=%llu requested=%llu weight=%llu\n",
            pointer, static_cast<unsigned long>(allocation_error), static_cast<unsigned long long>(sampler.hookCalls()),
            static_cast<unsigned long long>(hooks_before),
            static_cast<unsigned long long>(sampler.successfulAllocationCalls()),
            static_cast<unsigned long long>(successful_before), static_cast<unsigned long long>(sampler.liveSamples()),
            static_cast<unsigned long long>(live_before), allocation_lookup ? 1 : 0, allocation_state.found ? 1 : 0,
            static_cast<unsigned long long>(allocation_state.allocation_id),
            static_cast<unsigned long long>(allocation_state.requested_bytes),
            static_cast<unsigned long long>(allocation_state.weight_bytes));
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        return fail("hooked-small-allocation") ? 0 : 1;
    }
    std::memset(pointer, 0x5A, KSmallSize);

    ::SetLastError(KEntryError);
    void *expanded = fixture_realloc(heap, 0, pointer, KExpandedSize);
    const DWORD expanded_error = ::GetLastError();
    if (expanded == nullptr || expanded_error != KEntryError || !hasPattern(expanded, KSmallSize, 0x5A)) {
        if (expanded != nullptr) {
            (void)fixture_free(heap, 0, expanded);
        }
        else {
            (void)fixture_free(heap, 0, pointer);
        }
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        return fail("hooked-successful-realloc") ? 0 : 1;
    }
    pointer = expanded;
    std::memset(pointer, 0x5A, KExpandedSize);

    spark::test::AllocationLiveRecordState live_before_failed_realloc_state;
    const bool live_before_failed_realloc_lookup = spark::test::AllocationDiagnosticsTestAccess::liveRecordState(
        sampler, pointer, live_before_failed_realloc_state);
    if (!live_before_failed_realloc_lookup || !validLiveRecord(live_before_failed_realloc_state, KExpandedSize)) {
        std::fprintf(stderr,
                     "stage=windows-allocation-heap detail=failed-realloc-before lookup=%d found=%d id=%llu "
                     "requested=%llu weight=%llu\n",
                     live_before_failed_realloc_lookup ? 1 : 0, live_before_failed_realloc_state.found ? 1 : 0,
                     static_cast<unsigned long long>(live_before_failed_realloc_state.allocation_id),
                     static_cast<unsigned long long>(live_before_failed_realloc_state.requested_bytes),
                     static_cast<unsigned long long>(live_before_failed_realloc_state.weight_bytes));
        (void)fixture_free(heap, 0, pointer);
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        return fail("hooked-failed-realloc-record-before") ? 0 : 1;
    }
    const std::uint64_t live_before_failed_realloc = sampler.liveSamples();
    ::SetLastError(KEntryError);
    void *failed = nullptr;
    DWORD hooked_realloc_code = 0;
    const bool hooked_realloc_caught = invokeHeapException(
        [&] { failed = fixture_realloc(heap, KCallNormalFlags, pointer, KImpossibleSize); }, hooked_realloc_code);
    const DWORD failure_error = ::GetLastError();
    spark::test::AllocationLiveRecordState live_after_failed_realloc_state;
    const bool live_after_failed_realloc_lookup = spark::test::AllocationDiagnosticsTestAccess::liveRecordState(
        sampler, pointer, live_after_failed_realloc_state);
    const bool record_survived = live_after_failed_realloc_lookup &&
                                 sameLiveRecord(live_before_failed_realloc_state, live_after_failed_realloc_state);
    if (!hooked_realloc_caught || failed != nullptr || hooked_realloc_code != direct_realloc_code ||
        failure_error != direct_failure_error || !record_survived) {
        std::fprintf(stderr,
                     "stage=windows-allocation-heap detail=failed-realloc direct-code=0x%08lx hooked-code=0x%08lx "
                     "direct-error=%lu hooked-error=%lu live=%llu/%llu caught=%d/%d before-lookup=%d "
                     "before-found=%d before-id=%llu before-requested=%llu before-weight=%llu after-lookup=%d "
                     "after-found=%d after-id=%llu after-requested=%llu after-weight=%llu\n",
                     static_cast<unsigned long>(direct_realloc_code), static_cast<unsigned long>(hooked_realloc_code),
                     static_cast<unsigned long>(direct_failure_error), static_cast<unsigned long>(failure_error),
                     static_cast<unsigned long long>(sampler.liveSamples()),
                     static_cast<unsigned long long>(live_before_failed_realloc), hooked_realloc_caught ? 1 : 0,
                     direct_realloc_caught ? 1 : 0, live_before_failed_realloc_lookup ? 1 : 0,
                     live_before_failed_realloc_state.found ? 1 : 0,
                     static_cast<unsigned long long>(live_before_failed_realloc_state.allocation_id),
                     static_cast<unsigned long long>(live_before_failed_realloc_state.requested_bytes),
                     static_cast<unsigned long long>(live_before_failed_realloc_state.weight_bytes),
                     live_after_failed_realloc_lookup ? 1 : 0, live_after_failed_realloc_state.found ? 1 : 0,
                     static_cast<unsigned long long>(live_after_failed_realloc_state.allocation_id),
                     static_cast<unsigned long long>(live_after_failed_realloc_state.requested_bytes),
                     static_cast<unsigned long long>(live_after_failed_realloc_state.weight_bytes));
        if (failed != nullptr) {
            (void)fixture_free(heap, 0, failed);
        }
        else {
            (void)fixture_free(heap, 0, pointer);
        }
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        return fail("hooked-failed-realloc") ? 0 : 1;
    }
    for (SIZE_T index = 0; index < KExpandedSize; ++index) {
        if (static_cast<const unsigned char *>(pointer)[index] != 0x5A) {
            (void)fixture_free(heap, 0, pointer);
            (void)sampler.shutdown(error);
            (void)::FreeLibrary(fixture);
            ::HeapDestroy(heap);
            return fail("failed-realloc-preserved-memory") ? 0 : 1;
        }
    }

    const bool freed = fixture_free(heap, 0, pointer) != 0;
    spark::test::AllocationLiveRecordState live_after_free_state;
    const bool live_after_free_lookup =
        spark::test::AllocationDiagnosticsTestAccess::liveRecordState(sampler, pointer, live_after_free_state);
    if (!freed || !live_after_free_lookup || live_after_free_state.found) {
        std::fprintf(stderr,
                     "stage=windows-allocation-heap detail=failed-realloc-free freed=%d lookup=%d found=%d id=%llu "
                     "requested=%llu weight=%llu live=%llu/%llu\n",
                     freed ? 1 : 0, live_after_free_lookup ? 1 : 0, live_after_free_state.found ? 1 : 0,
                     static_cast<unsigned long long>(live_after_free_state.allocation_id),
                     static_cast<unsigned long long>(live_after_free_state.requested_bytes),
                     static_cast<unsigned long long>(live_after_free_state.weight_bytes),
                     static_cast<unsigned long long>(sampler.liveSamples()),
                     static_cast<unsigned long long>(live_before_failed_realloc));
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        return fail("hooked-free-after-failure") ? 0 : 1;
    }

    void *subsequent = fixture_alloc(heap, 0, KSmallSize);
    if (subsequent == nullptr || !fixture_free(heap, 0, subsequent)) {
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        return fail("subsequent-tracked-allocation") ? 0 : 1;
    }

    HANDLE second_heap = ::HeapCreate(0, 0, 64 * 1024);
    if (second_heap == nullptr) {
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        return fail("second-heap-create") ? 0 : 1;
    }
    DWORD second_direct_alloc_code = 0;
    void *second_direct_failed_alloc = nullptr;
    ::SetLastError(KEntryError);
    const bool second_direct_alloc_caught = invokeHeapException(
        [&] { second_direct_failed_alloc = direct_alloc(second_heap, KCallExceptionFlags, KImpossibleSize); },
        second_direct_alloc_code);
    const DWORD second_direct_alloc_error = ::GetLastError();
    DWORD second_hooked_alloc_code = 0;
    void *second_hooked_failed_alloc = nullptr;
    ::SetLastError(KEntryError);
    const bool second_hooked_alloc_caught = invokeHeapException(
        [&] { second_hooked_failed_alloc = fixture_alloc(second_heap, KCallExceptionFlags, KImpossibleSize); },
        second_hooked_alloc_code);
    const DWORD second_hooked_alloc_error = ::GetLastError();
    if (!second_direct_alloc_caught || second_direct_failed_alloc != nullptr || !second_hooked_alloc_caught ||
        second_hooked_failed_alloc != nullptr || second_direct_alloc_code != second_hooked_alloc_code ||
        second_direct_alloc_error != second_hooked_alloc_error) {
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(second_heap);
        ::HeapDestroy(heap);
        return fail("second-heap-allocation-mode") ? 0 : 1;
    }

    void *second_direct_pointer = direct_alloc(second_heap, 0, KSmallSize);
    if (second_direct_pointer == nullptr) {
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(second_heap);
        ::HeapDestroy(heap);
        return fail("second-direct-small-allocation") ? 0 : 1;
    }
    std::memset(second_direct_pointer, 0xA5, KSmallSize);
    DWORD second_direct_realloc_code = 0;
    void *second_direct_failed = nullptr;
    ::SetLastError(KEntryError);
    const bool second_direct_realloc_caught = invokeHeapException(
        [&] {
            second_direct_failed =
                direct_realloc(second_heap, KCallExceptionFlags, second_direct_pointer, KImpossibleSize);
        },
        second_direct_realloc_code);
    const DWORD second_direct_realloc_error = ::GetLastError();
    if (!second_direct_realloc_caught || second_direct_failed != nullptr ||
        !hasPattern(second_direct_pointer, KSmallSize, 0xA5)) {
        (void)direct_free(second_heap, 0,
                          second_direct_failed != nullptr ? second_direct_failed : second_direct_pointer);
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(second_heap);
        ::HeapDestroy(heap);
        return fail("second-direct-realloc-mode") ? 0 : 1;
    }
    (void)direct_free(second_heap, 0, second_direct_pointer);

    void *second_fixture_pointer = fixture_alloc(second_heap, 0, KSmallSize);
    if (second_fixture_pointer == nullptr) {
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(second_heap);
        ::HeapDestroy(heap);
        return fail("second-fixture-small-allocation") ? 0 : 1;
    }
    std::memset(second_fixture_pointer, 0x5A, KSmallSize);
    spark::test::AllocationLiveRecordState second_live_before_realloc_state;
    const bool second_live_before_realloc_lookup = spark::test::AllocationDiagnosticsTestAccess::liveRecordState(
        sampler, second_fixture_pointer, second_live_before_realloc_state);
    if (!second_live_before_realloc_lookup || !validLiveRecord(second_live_before_realloc_state, KSmallSize)) {
        std::fprintf(stderr,
                     "stage=windows-allocation-heap detail=second-realloc-before lookup=%d found=%d id=%llu "
                     "requested=%llu weight=%llu\n",
                     second_live_before_realloc_lookup ? 1 : 0, second_live_before_realloc_state.found ? 1 : 0,
                     static_cast<unsigned long long>(second_live_before_realloc_state.allocation_id),
                     static_cast<unsigned long long>(second_live_before_realloc_state.requested_bytes),
                     static_cast<unsigned long long>(second_live_before_realloc_state.weight_bytes));
        (void)fixture_free(second_heap, 0, second_fixture_pointer);
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(second_heap);
        ::HeapDestroy(heap);
        return fail("second-fixture-realloc-record-before") ? 0 : 1;
    }
    const std::uint64_t second_live_before_realloc = sampler.liveSamples();
    DWORD second_hooked_realloc_code = 0;
    void *second_hooked_failed = nullptr;
    ::SetLastError(KEntryError);
    const bool second_hooked_realloc_caught = invokeHeapException(
        [&] {
            second_hooked_failed =
                fixture_realloc(second_heap, KCallExceptionFlags, second_fixture_pointer, KImpossibleSize);
        },
        second_hooked_realloc_code);
    const DWORD second_hooked_realloc_error = ::GetLastError();
    spark::test::AllocationLiveRecordState second_live_after_realloc_state;
    const bool second_live_after_realloc_lookup = spark::test::AllocationDiagnosticsTestAccess::liveRecordState(
        sampler, second_fixture_pointer, second_live_after_realloc_state);
    const bool second_record_survived =
        second_live_after_realloc_lookup &&
        sameLiveRecord(second_live_before_realloc_state, second_live_after_realloc_state);
    if (!second_hooked_realloc_caught || second_hooked_failed != nullptr ||
        second_hooked_realloc_code != second_direct_realloc_code ||
        second_hooked_realloc_error != second_direct_realloc_error || !second_record_survived ||
        !hasPattern(second_fixture_pointer, KSmallSize, 0x5A)) {
        std::fprintf(stderr,
                     "stage=windows-allocation-heap detail=second-realloc direct-code=0x%08lx hooked-code=0x%08lx "
                     "direct-error=%lu hooked-error=%lu live=%llu/%llu caught=%d/%d before-lookup=%d before-found=%d "
                     "before-id=%llu before-requested=%llu before-weight=%llu after-lookup=%d after-found=%d "
                     "after-id=%llu after-requested=%llu after-weight=%llu\n",
                     static_cast<unsigned long>(second_direct_realloc_code),
                     static_cast<unsigned long>(second_hooked_realloc_code),
                     static_cast<unsigned long>(second_direct_realloc_error),
                     static_cast<unsigned long>(second_hooked_realloc_error),
                     static_cast<unsigned long long>(sampler.liveSamples()),
                     static_cast<unsigned long long>(second_live_before_realloc), second_direct_realloc_caught ? 1 : 0,
                     second_hooked_realloc_caught ? 1 : 0, second_live_before_realloc_lookup ? 1 : 0,
                     second_live_before_realloc_state.found ? 1 : 0,
                     static_cast<unsigned long long>(second_live_before_realloc_state.allocation_id),
                     static_cast<unsigned long long>(second_live_before_realloc_state.requested_bytes),
                     static_cast<unsigned long long>(second_live_before_realloc_state.weight_bytes),
                     second_live_after_realloc_lookup ? 1 : 0, second_live_after_realloc_state.found ? 1 : 0,
                     static_cast<unsigned long long>(second_live_after_realloc_state.allocation_id),
                     static_cast<unsigned long long>(second_live_after_realloc_state.requested_bytes),
                     static_cast<unsigned long long>(second_live_after_realloc_state.weight_bytes));
        (void)fixture_free(second_heap, 0,
                           second_hooked_failed != nullptr ? second_hooked_failed : second_fixture_pointer);
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(second_heap);
        ::HeapDestroy(heap);
        return fail("second-fixture-realloc-mode") ? 0 : 1;
    }
    const bool second_freed = fixture_free(second_heap, 0, second_fixture_pointer) != 0;
    spark::test::AllocationLiveRecordState second_live_after_free_state;
    const bool second_live_after_free_lookup = spark::test::AllocationDiagnosticsTestAccess::liveRecordState(
        sampler, second_fixture_pointer, second_live_after_free_state);
    if (!second_freed || !second_live_after_free_lookup || second_live_after_free_state.found) {
        std::fprintf(stderr,
                     "stage=windows-allocation-heap detail=second-realloc-free freed=%d lookup=%d found=%d id=%llu "
                     "requested=%llu weight=%llu live=%llu/%llu\n",
                     second_freed ? 1 : 0, second_live_after_free_lookup ? 1 : 0,
                     second_live_after_free_state.found ? 1 : 0,
                     static_cast<unsigned long long>(second_live_after_free_state.allocation_id),
                     static_cast<unsigned long long>(second_live_after_free_state.requested_bytes),
                     static_cast<unsigned long long>(second_live_after_free_state.weight_bytes),
                     static_cast<unsigned long long>(sampler.liveSamples()),
                     static_cast<unsigned long long>(second_live_before_realloc));
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(second_heap);
        ::HeapDestroy(heap);
        return fail("second-fixture-realloc-free") ? 0 : 1;
    }
    ::HeapDestroy(second_heap);

    if (!sampler.stop(error) || !error.empty() || !sampler.shutdown(error) || !error.empty()) {
        (void)::FreeLibrary(fixture);
        ::HeapDestroy(heap);
        return fail("sampler-cleanup") ? 0 : 1;
    }
    if (!::FreeLibrary(fixture) || !::HeapDestroy(heap)) {
        return fail("heap-destroy") ? 0 : 1;
    }
    std::fprintf(stderr, "stage=windows-allocation-heap pass\n");
    return 0;
}
