#include "native/alloc/windows_permanent_iat_gateway.h"

#ifndef _WIN32
#error "windows_permanent_iat_gateway_seh_test.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

using spark::permanent_iat_gateway::bindPermanentIatGateway;
using spark::permanent_iat_gateway::createPermanentIatGateway;
using spark::permanent_iat_gateway::detachPermanentIatGateway;
using spark::permanent_iat_gateway::permanentIatGatewayActive;
using spark::permanent_iat_gateway::PermanentIatGatewayHandle;

namespace {

using FourArgFn = std::uint64_t(__cdecl *)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using FiveArgFn = std::uint64_t(__cdecl *)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

constexpr DWORD KNoncontinuableCode = 0xE0420001;
constexpr DWORD KContinuableCode = 0xE0420002;
constexpr std::uint64_t KHandlerBias = 0x100000000ULL;

std::atomic<std::uint64_t> GFourthArgument{0};
std::atomic<std::uint64_t> GFifthArgument{0};
std::atomic<std::uint64_t> GContinuableCalls{0};

struct ExceptionObservation {
    DWORD code = 0;
    DWORD flags = 0;
    DWORD arguments = 0;
    ULONG_PTR information[3]{};
};

PermanentIatGatewayHandle *GContinuableGateway = nullptr;
std::atomic<std::uint64_t> GContinuableFilterActive{0};

[[nodiscard]] std::uint64_t fourArgValue(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    return a + 3 * b + 5 * c + 7 * d;
}

[[nodiscard]] std::uint64_t fiveArgValue(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                                         std::uint64_t e) noexcept
{
    return a + 3 * b + 5 * c + 7 * d + 11 * e;
}

extern "C" __declspec(noinline) std::uint64_t __cdecl originalFour(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                   std::uint64_t d) noexcept
{
    return fourArgValue(a, b, c, d);
}

extern "C" __declspec(noinline) std::uint64_t __cdecl originalFive(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                   std::uint64_t d, std::uint64_t e) noexcept
{
    return fiveArgValue(a, b, c, d, e);
}

extern "C" __declspec(noinline) std::uint64_t __cdecl throwingFour(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                   std::uint64_t d) noexcept
{
    GFourthArgument.store(d, std::memory_order_release);
    const ULONG_PTR information[3] = {0xF0F0F0F0ULL, 0x01020304ULL, 0x55667788ULL};
    ::RaiseException(KNoncontinuableCode, EXCEPTION_NONCONTINUABLE, 3, information);
    return fourArgValue(a, b, c, d);
}

extern "C" __declspec(noinline) std::uint64_t __cdecl throwingFive(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                   std::uint64_t d, std::uint64_t e) noexcept
{
    GFifthArgument.store(e, std::memory_order_release);
    const ULONG_PTR information[3] = {0xA0A0A0A0ULL, 0x11223344ULL, 0x99AABBCCULL};
    ::RaiseException(KNoncontinuableCode, EXCEPTION_NONCONTINUABLE, 3, information);
    return fiveArgValue(a, b, c, d, e);
}

extern "C" __declspec(noinline) std::uint64_t __cdecl handledFive(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                  std::uint64_t d, std::uint64_t e) noexcept
{
    GContinuableCalls.fetch_add(1, std::memory_order_relaxed);
    const ULONG_PTR information[2] = {0x12345678ULL, 0xCAFEBABEULL};
    ::RaiseException(KContinuableCode, 0, 2, information);
    return fiveArgValue(a, b, c, d, e) + KHandlerBias;
}

LONG WINAPI continueExecutionFilter(EXCEPTION_POINTERS *exception) noexcept
{
    if (exception == nullptr || exception->ExceptionRecord == nullptr || GContinuableGateway == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;
    if (record->ExceptionCode != KContinuableCode ||
        (record->ExceptionFlags &
         (EXCEPTION_NONCONTINUABLE | EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND | EXCEPTION_TARGET_UNWIND)) != 0 ||
        record->NumberParameters != 2 || record->ExceptionInformation[0] != 0x12345678ULL ||
        record->ExceptionInformation[1] != 0xCAFEBABEULL) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    GContinuableFilterActive.store(permanentIatGatewayActive(*GContinuableGateway), std::memory_order_release);
    return EXCEPTION_CONTINUE_EXECUTION;
}

int captureException(ExceptionObservation *observation, EXCEPTION_POINTERS *exception) noexcept
{
    if (observation != nullptr && exception != nullptr && exception->ExceptionRecord != nullptr) {
        const EXCEPTION_RECORD *record = exception->ExceptionRecord;
        observation->code = record->ExceptionCode;
        observation->flags = record->ExceptionFlags;
        observation->arguments = record->NumberParameters;
        for (DWORD index = 0; index < (std::min)(record->NumberParameters, 3UL); ++index) {
            observation->information[index] = record->ExceptionInformation[index];
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

template <typename Function>
[[nodiscard]] bool invokeCaught(Function function, ExceptionObservation &observation) noexcept
{
    observation = {};
    __try {
        if constexpr (std::is_same_v<Function, FourArgFn>) {
            (void)function(1, 2, 3, 4);
        }
        else {
            (void)function(5, 6, 7, 8, 9);
        }
    }
    __except (captureException(&observation, GetExceptionInformation())) {
        return true;
    }
    return false;
}

bool require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "stage=permanent-iat-gateway-seh failure=%s\n", message);
        return false;
    }
    return true;
}

bool requireGatewayEncoding(const PermanentIatGatewayHandle &handle)
{
    const auto *code = static_cast<const std::uint8_t *>(handle.gateway);
    std::size_t unwind_offset = 0;
    for (std::size_t offset = 0; offset + 12 <= 512; offset += 4) {
        constexpr std::uint8_t header[] = {0x11, 0x04, 0x01, 0x00, 0x04, 0x62, 0x00, 0x00};
        if (std::memcmp(code + offset, header, sizeof(header)) == 0) {
            unwind_offset = offset;
            break;
        }
    }
    if (!require(unwind_offset != 0, "unwind-header")) {
        return false;
    }

    DWORD handler_rva = 0;
    std::memcpy(&handler_rva, code + unwind_offset + 8, sizeof(handler_rva));
    if (!require(handler_rva < unwind_offset && handler_rva < 256, "handler-rva")) {
        return false;
    }

    std::size_t call_stub = 0;
    for (std::size_t offset = handler_rva; offset + 4 < unwind_offset; ++offset) {
        constexpr std::uint8_t prologue[] = {0x48, 0x83, 0xEC, 0x38};
        if (std::memcmp(code + offset, prologue, sizeof(prologue)) == 0) {
            call_stub = offset;
            break;
        }
    }
    if (!require(call_stub != 0, "call-stub-prologue")) {
        return false;
    }
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION runtime_function =
        ::RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(handle.gateway) + call_stub + 4, &image_base, nullptr);
    return require(runtime_function != nullptr && image_base == reinterpret_cast<DWORD64>(handle.gateway) &&
                       runtime_function->BeginAddress == call_stub && runtime_function->UnwindData == unwind_offset,
                   "call-stub-runtime-function");
}

bool runFourArgumentNoncontinuable()
{
    PermanentIatGatewayHandle handle;
    std::string error;
    if (!require(createPermanentIatGateway(reinterpret_cast<void *>(&originalFour), 0, handle, error), "four-create")) {
        return false;
    }
    if (!requireGatewayEncoding(handle)) {
        return false;
    }
    if (!require(bindPermanentIatGateway(handle, reinterpret_cast<void *>(&throwingFour), 5000, error), "four-bind")) {
        return false;
    }

    ExceptionObservation observation;
    const bool caught = invokeCaught(reinterpret_cast<FourArgFn>(handle.gateway), observation);
    if (!require(caught && observation.code == KNoncontinuableCode &&
                     (observation.flags & EXCEPTION_NONCONTINUABLE) != 0 && observation.arguments == 3 &&
                     observation.information[0] == 0xF0F0F0F0ULL && observation.information[1] == 0x01020304ULL &&
                     observation.information[2] == 0x55667788ULL,
                 "four-exception")) {
        return false;
    }
    if (!require(GFourthArgument.load(std::memory_order_acquire) == 4, "four-argument")) {
        return false;
    }
    if (!require(permanentIatGatewayActive(handle) == 0, "four-active-cleanup")) {
        return false;
    }
    return require(detachPermanentIatGateway(handle, 5000, error), "four-detach");
}

bool runFiveArgumentNoncontinuable()
{
    PermanentIatGatewayHandle handle;
    std::string error;
    if (!require(createPermanentIatGateway(reinterpret_cast<void *>(&originalFive), 1, handle, error), "five-create")) {
        return false;
    }
    if (!requireGatewayEncoding(handle)) {
        return false;
    }
    if (!require(bindPermanentIatGateway(handle, reinterpret_cast<void *>(&throwingFive), 5000, error), "five-bind")) {
        return false;
    }

    ExceptionObservation observation;
    const bool caught = invokeCaught(reinterpret_cast<FiveArgFn>(handle.gateway), observation);
    if (!require(caught && observation.code == KNoncontinuableCode &&
                     (observation.flags & EXCEPTION_NONCONTINUABLE) != 0 && observation.arguments == 3 &&
                     observation.information[0] == 0xA0A0A0A0ULL && observation.information[1] == 0x11223344ULL &&
                     observation.information[2] == 0x99AABBCCULL,
                 "five-exception")) {
        return false;
    }
    if (!require(GFifthArgument.load(std::memory_order_acquire) == 9, "five-argument")) {
        return false;
    }
    if (!require(permanentIatGatewayActive(handle) == 0, "five-active-cleanup")) {
        return false;
    }
    return require(detachPermanentIatGateway(handle, 5000, error), "five-detach");
}

bool runContinuableFiveArgument()
{
    PermanentIatGatewayHandle handle;
    std::string error;
    if (!require(createPermanentIatGateway(reinterpret_cast<void *>(&originalFive), 1, handle, error),
                 "continuable-create")) {
        return false;
    }
    if (!require(bindPermanentIatGateway(handle, reinterpret_cast<void *>(&handledFive), 5000, error),
                 "continuable-bind")) {
        return false;
    }

    GContinuableGateway = &handle;
    GContinuableFilterActive.store(0, std::memory_order_release);
    const std::uint64_t expected = fiveArgValue(5, 6, 7, 8, 9) + KHandlerBias;
    std::uint64_t result = 0;
    __try {
        result = reinterpret_cast<FiveArgFn>(handle.gateway)(5, 6, 7, 8, 9);
    }
    __except (continueExecutionFilter(GetExceptionInformation())) {
        return require(false, "continuable-filter-search");
    }
    GContinuableGateway = nullptr;
    if (!require(result == expected && GContinuableCalls.load(std::memory_order_acquire) == 1 &&
                     GContinuableFilterActive.load(std::memory_order_acquire) == 1,
                 "continuable-result")) {
        return false;
    }
    if (!require(permanentIatGatewayActive(handle) == 0, "continuable-active-cleanup")) {
        return false;
    }
    return require(detachPermanentIatGateway(handle, 5000, error), "continuable-detach");
}

}  // namespace

int main()
{
    return runFourArgumentNoncontinuable() && runFiveArgumentNoncontinuable() && runContinuableFiveArgument() ? 0 : 1;
}
