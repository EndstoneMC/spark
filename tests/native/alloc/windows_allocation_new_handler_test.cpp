#include "native/alloc/allocation_sampler.h"

#ifndef _WIN32
#error "windows_allocation_new_handler_test.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <malloc.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace {

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
using NewHandlerFn = int(__cdecl *)(std::size_t);
using SetNewModeFn = int(__cdecl *)(int);
using QueryNewModeFn = int(__cdecl *)();
using SetNewHandlerFn = NewHandlerFn(__cdecl *)(NewHandlerFn);
using QueryNewHandlerFn = NewHandlerFn(__cdecl *)();

using FixtureMallocFn = void *(*)(std::size_t);
using FixtureCallocFn = void *(*)(std::size_t, std::size_t);
using FixtureReallocFn = void *(*)(void *, std::size_t);
using FixtureRecallocFn = void *(*)(void *, std::size_t, std::size_t);
using FixtureAlignedMallocFn = void *(*)(std::size_t, std::size_t);
using FixtureAlignedReallocFn = void *(*)(void *, std::size_t, std::size_t);
using FixtureAlignedRecallocFn = void *(*)(void *, std::size_t, std::size_t, std::size_t);
using FixtureAlignedOffsetMallocFn = void *(*)(std::size_t, std::size_t, std::size_t);
using FixtureAlignedOffsetReallocFn = void *(*)(void *, std::size_t, std::size_t, std::size_t);
using FixtureAlignedOffsetRecallocFn = void *(*)(void *, std::size_t, std::size_t, std::size_t, std::size_t);
using FixtureFreeFn = void (*)(void *);
using FixtureAlignedFreeFn = void (*)(void *);

constexpr DWORD kEntryError = 0x13572468;
constexpr std::size_t kSmallSize = 64;
constexpr std::size_t kSmallAlignment = 64;
constexpr std::size_t kSmallOffset = 5;
constexpr std::size_t kImpossibleSize = std::size_t{1} << 60;
constexpr std::size_t kHeapMaxReq = 0xFFFFFFFFFFFFFFE0ULL;

std::atomic<unsigned> g_new_handler_calls{0};
std::array<void *, 16> g_gateway_states{};
std::size_t g_gateway_state_count = 0;
std::atomic<bool> g_gateway_active_observed{false};

class NewHandlerMarker final : public std::bad_alloc {
public:
    [[nodiscard]] const char *what() const noexcept override { return "spark real new-handler marker"; }
};

int __cdecl throwingNewHandler(std::size_t)
{
    bool active = false;
    for (std::size_t index = 0; index < g_gateway_state_count; ++index) {
        const auto state = static_cast<const std::uint8_t *>(g_gateway_states[index]);
        std::uint64_t gate = 0;
        std::uint64_t active_calls = 0;
        void *handler = nullptr;
        std::memcpy(&gate, state + 24, sizeof(gate));
        std::memcpy(&active_calls, state + 32, sizeof(active_calls));
        std::memcpy(&handler, state + 40, sizeof(handler));
        if (gate == 1 && active_calls != 0 && handler != nullptr) {
            active = true;
            break;
        }
    }
    g_gateway_active_observed.store(active, std::memory_order_release);
    g_new_handler_calls.fetch_add(1, std::memory_order_relaxed);
    throw NewHandlerMarker{};
}

template <typename Function>
Function exportFunction(HMODULE module, const char *name) noexcept
{
    return reinterpret_cast<Function>(::GetProcAddress(module, name));
}

struct UcrtExports {
    MallocFn malloc = nullptr;
    CallocFn calloc = nullptr;
    ReallocFn realloc = nullptr;
    RecallocFn recalloc = nullptr;
    AlignedMallocFn aligned_malloc = nullptr;
    AlignedReallocFn aligned_realloc = nullptr;
    AlignedRecallocFn aligned_recalloc = nullptr;
    AlignedOffsetMallocFn aligned_offset_malloc = nullptr;
    AlignedOffsetReallocFn aligned_offset_realloc = nullptr;
    AlignedOffsetRecallocFn aligned_offset_recalloc = nullptr;
    MallocFn malloc_base = nullptr;
    CallocFn calloc_base = nullptr;
    ReallocFn realloc_base = nullptr;
    FreeFn free = nullptr;
    FreeFn aligned_free = nullptr;
    SetNewModeFn set_new_mode = nullptr;
    QueryNewModeFn query_new_mode = nullptr;
    SetNewHandlerFn set_new_handler = nullptr;
    QueryNewHandlerFn query_new_handler = nullptr;
};

struct FixtureExports {
    FixtureMallocFn malloc = nullptr;
    FixtureCallocFn calloc = nullptr;
    FixtureReallocFn realloc = nullptr;
    FixtureRecallocFn recalloc = nullptr;
    FixtureAlignedMallocFn aligned_malloc = nullptr;
    FixtureAlignedReallocFn aligned_realloc = nullptr;
    FixtureAlignedRecallocFn aligned_recalloc = nullptr;
    FixtureAlignedOffsetMallocFn aligned_offset_malloc = nullptr;
    FixtureAlignedOffsetReallocFn aligned_offset_realloc = nullptr;
    FixtureAlignedOffsetRecallocFn aligned_offset_recalloc = nullptr;
    FixtureMallocFn malloc_base = nullptr;
    FixtureCallocFn calloc_base = nullptr;
    FixtureReallocFn realloc_base = nullptr;
    FixtureFreeFn free = nullptr;
    FixtureAlignedFreeFn aligned_free = nullptr;
};

struct GatewayImportExpectation {
    const char *name = nullptr;
    void *original = nullptr;
};

bool importSlot(HMODULE module, const char *import_name, void *&slot_value, void *&slot_address) noexcept
{
    const auto *base = reinterpret_cast<const std::uint8_t *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        return false;
    }
    const auto *descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    for (const IMAGE_IMPORT_DESCRIPTOR *descriptor = descriptors; descriptor->Name != 0; ++descriptor) {
        const auto *lookup = reinterpret_cast<const IMAGE_THUNK_DATA64 *>(base + descriptor->OriginalFirstThunk);
        auto *address =
            reinterpret_cast<IMAGE_THUNK_DATA64 *>(const_cast<std::uint8_t *>(base) + descriptor->FirstThunk);
        if (lookup == nullptr || address == nullptr) {
            continue;
        }
        for (std::size_t index = 0; lookup[index].u1.AddressOfData != 0; ++index) {
            if (IMAGE_SNAP_BY_ORDINAL64(lookup[index].u1.Ordinal)) {
                continue;
            }
            const auto *name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + lookup[index].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(name->Name), import_name) != 0) {
                continue;
            }
            auto *slot = &address[index].u1.Function;
            std::atomic_ref<std::uintptr_t> atomic_slot(*slot);
            slot_value = reinterpret_cast<void *>(atomic_slot.load(std::memory_order_acquire));
            slot_address = slot;
            return true;
        }
    }
    return false;
}

bool inspectGatewayImports(HMODULE fixture, const UcrtExports &direct, const char *&failure) noexcept
{
    const GatewayImportExpectation expectations[] = {
        {"malloc", reinterpret_cast<void *>(direct.malloc)},
        {"calloc", reinterpret_cast<void *>(direct.calloc)},
        {"realloc", reinterpret_cast<void *>(direct.realloc)},
        {"_recalloc", reinterpret_cast<void *>(direct.recalloc)},
        {"_aligned_malloc", reinterpret_cast<void *>(direct.aligned_malloc)},
        {"_aligned_realloc", reinterpret_cast<void *>(direct.aligned_realloc)},
        {"_aligned_recalloc", reinterpret_cast<void *>(direct.aligned_recalloc)},
        {"_aligned_offset_malloc", reinterpret_cast<void *>(direct.aligned_offset_malloc)},
        {"_aligned_offset_realloc", reinterpret_cast<void *>(direct.aligned_offset_realloc)},
        {"_aligned_offset_recalloc", reinterpret_cast<void *>(direct.aligned_offset_recalloc)},
        {"_malloc_base", reinterpret_cast<void *>(direct.malloc_base)},
        {"_calloc_base", reinterpret_cast<void *>(direct.calloc_base)},
        {"_realloc_base", reinterpret_cast<void *>(direct.realloc_base)},
    };
    g_gateway_state_count = 0;
    for (const auto &expectation : expectations) {
        void *slot_value = nullptr;
        void *slot_address = nullptr;
        if (!importSlot(fixture, expectation.name, slot_value, slot_address)) {
            failure = expectation.name;
            return false;
        }
        if (slot_value == nullptr || slot_value == expectation.original || slot_address == nullptr) {
            failure = expectation.name;
            return false;
        }
        const auto *gateway_bytes = static_cast<const std::uint8_t *>(slot_value);
        if (gateway_bytes[0] != 0x49 || gateway_bytes[1] != 0xBB) {
            failure = expectation.name;
            return false;
        }
        void *state = nullptr;
        std::memcpy(&state, gateway_bytes + 2, sizeof(state));
        if (state == nullptr) {
            failure = expectation.name;
            return false;
        }
        std::uint64_t magic = 0;
        std::uint32_t abi = 0;
        void *gateway_identity = nullptr;
        std::memcpy(&magic, state, sizeof(magic));
        std::memcpy(&abi, static_cast<const std::uint8_t *>(state) + 8, sizeof(abi));
        std::memcpy(&gateway_identity, static_cast<const std::uint8_t *>(state) + 56, sizeof(gateway_identity));
        if (magic != 0x3154414947504B53ULL || abi != 3 || gateway_identity != slot_value) {
            failure = expectation.name;
            return false;
        }
        if (g_gateway_state_count >= g_gateway_states.size()) {
            failure = "gateway-state-capacity";
            return false;
        }
        g_gateway_states[g_gateway_state_count++] = state;
    }
    return true;
}

struct NewHandlerState {
    UcrtExports &ucrt;
    const int previous_mode;
    NewHandlerFn const previous_handler;
    bool active = false;

    void enable() noexcept
    {
        (void)ucrt.set_new_handler(&throwingNewHandler);
        (void)ucrt.set_new_mode(1);
        active = true;
    }

    void restore() noexcept
    {
        if (!active) {
            return;
        }
        (void)ucrt.set_new_mode(previous_mode);
        (void)ucrt.set_new_handler(previous_handler);
        active = false;
    }

    ~NewHandlerState() { restore(); }
};

bool fail(const char *message)
{
    std::fprintf(stderr, "stage=windows-allocation-new-handler failure=%s\n", message);
    return false;
}

template <typename Call>
bool runThrowingCall(NewHandlerState &state, const char *name, Call call, DWORD &last_error, bool expect_gateway_active,
                     bool check_gateway_active)
{
    g_new_handler_calls.store(0, std::memory_order_relaxed);
    g_gateway_active_observed.store(false, std::memory_order_release);
    state.enable();
    ::SetLastError(kEntryError);
    bool caught = false;
    try {
        (void)call();
    }
    catch (const NewHandlerMarker &exception) {
        caught = std::strcmp(exception.what(), "spark real new-handler marker") == 0;
    }
    catch (...) {
    }
    last_error = ::GetLastError();
    const unsigned handler_calls = g_new_handler_calls.load(std::memory_order_relaxed);
    state.restore();
    if (!caught || handler_calls != 1 ||
        (check_gateway_active && g_gateway_active_observed.load(std::memory_order_acquire) != expect_gateway_active)) {
        std::fprintf(stderr, "stage=windows-allocation-new-handler detail=%s caught=%d calls=%u\n", name,
                     caught ? 1 : 0, handler_calls);
        return false;
    }
    return true;
}

template <typename DirectCall, typename FixtureCall>
bool runAllocationCase(spark::AllocationSampler &sampler, NewHandlerState &state, const char *name,
                       DirectCall direct_call, FixtureCall fixture_call)
{
    DWORD direct_error = 0;
    (void)sampler;
    if (!runThrowingCall(state, name, direct_call, direct_error, false, false)) {
        return false;
    }
    DWORD fixture_error = 0;
    if (!runThrowingCall(state, name, fixture_call, fixture_error, true, true)) {
        return false;
    }
    return direct_error == fixture_error || fail("real-vs-direct LastError mismatch");
}

template <typename DirectAlloc, typename DirectRealloc, typename DirectFree, typename FixtureAlloc,
          typename FixtureRealloc, typename FixtureFree>
bool runReallocCase(spark::AllocationSampler &sampler, NewHandlerState &state, const char *name,
                    DirectAlloc direct_alloc, DirectRealloc direct_realloc, DirectFree direct_free,
                    FixtureAlloc fixture_alloc, FixtureRealloc fixture_realloc, FixtureFree fixture_free)
{
    void *direct_pointer = direct_alloc();
    if (direct_pointer == nullptr) {
        return fail("direct realloc setup allocation failed");
    }
    std::memset(direct_pointer, 0xA5, kSmallSize);
    DWORD direct_error = 0;
    const bool direct_ok = runThrowingCall(
        state, name, [&] { return direct_realloc(direct_pointer, kImpossibleSize); }, direct_error, false, false);
    bool direct_preserved = direct_ok && direct_pointer != nullptr;
    if (direct_preserved) {
        const auto *bytes = static_cast<const unsigned char *>(direct_pointer);
        for (std::size_t index = 0; index < kSmallSize; ++index) {
            if (bytes[index] != 0xA5) {
                std::fprintf(stderr, "stage=windows-allocation-new-handler detail=%s direct-byte-index=%zu value=%u\n",
                             name, index, static_cast<unsigned>(bytes[index]));
                direct_preserved = false;
                break;
            }
        }
    }
    direct_free(direct_pointer);
    if (!direct_preserved) {
        return fail("direct realloc changed original memory");
    }

    void *fixture_pointer = nullptr;
    for (int attempt = 0; attempt < 64 && fixture_pointer == nullptr; ++attempt) {
        const std::uint64_t live_before_candidate = sampler.liveSamples();
        void *candidate = fixture_alloc();
        if (candidate == nullptr) {
            return fail("fixture realloc setup allocation failed");
        }
        if (sampler.liveSamples() > live_before_candidate) {
            fixture_pointer = candidate;
        }
        else {
            fixture_free(candidate);
        }
    }
    if (fixture_pointer == nullptr) {
        return fail("fixture realloc record was not observed before exception");
    }
    std::memset(fixture_pointer, 0x5A, kSmallSize);
    const std::uint64_t live_before_exception = sampler.liveSamples();
    DWORD fixture_error = 0;
    const bool fixture_ok = runThrowingCall(
        state, name, [&] { return fixture_realloc(fixture_pointer, kImpossibleSize); }, fixture_error, true, true);
    const bool record_survived = sampler.liveSamples() == live_before_exception;
    const bool fixture_preserved = fixture_ok && fixture_pointer != nullptr;
    if (fixture_preserved) {
        const auto *bytes = static_cast<const unsigned char *>(fixture_pointer);
        for (std::size_t index = 0; index < kSmallSize; ++index) {
            if (bytes[index] != 0x5A) {
                return fail("fixture realloc changed original memory");
            }
        }
    }
    fixture_free(fixture_pointer);
    if (!fixture_preserved || !record_survived || direct_error != fixture_error) {
        std::fprintf(stderr,
                     "stage=windows-allocation-new-handler detail=%s direct-error=%lu fixture-error=%lu record=%d "
                     "live=%llu before=%llu\n",
                     name, static_cast<unsigned long>(direct_error), static_cast<unsigned long>(fixture_error),
                     record_survived ? 1 : 0, static_cast<unsigned long long>(sampler.liveSamples()),
                     static_cast<unsigned long long>(live_before_exception));
    }
    return fixture_preserved && record_survived && direct_error == fixture_error ||
           fail("real-vs-direct realloc record/LastError mismatch");
}

}  // namespace

int main()
{
    HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
    HMODULE fixture_module = ::LoadLibraryW(L".\\spark_windows_allocation_fixture.dll");
    if (ucrt == nullptr || fixture_module == nullptr) {
        return fail("required-module") ? 0 : 1;
    }

    UcrtExports direct{
        .malloc = exportFunction<MallocFn>(ucrt, "malloc"),
        .calloc = exportFunction<CallocFn>(ucrt, "calloc"),
        .realloc = exportFunction<ReallocFn>(ucrt, "realloc"),
        .recalloc = exportFunction<RecallocFn>(ucrt, "_recalloc"),
        .aligned_malloc = exportFunction<AlignedMallocFn>(ucrt, "_aligned_malloc"),
        .aligned_realloc = exportFunction<AlignedReallocFn>(ucrt, "_aligned_realloc"),
        .aligned_recalloc = exportFunction<AlignedRecallocFn>(ucrt, "_aligned_recalloc"),
        .aligned_offset_malloc = exportFunction<AlignedOffsetMallocFn>(ucrt, "_aligned_offset_malloc"),
        .aligned_offset_realloc = exportFunction<AlignedOffsetReallocFn>(ucrt, "_aligned_offset_realloc"),
        .aligned_offset_recalloc = exportFunction<AlignedOffsetRecallocFn>(ucrt, "_aligned_offset_recalloc"),
        .malloc_base = exportFunction<MallocFn>(ucrt, "_malloc_base"),
        .calloc_base = exportFunction<CallocFn>(ucrt, "_calloc_base"),
        .realloc_base = exportFunction<ReallocFn>(ucrt, "_realloc_base"),
        .free = exportFunction<FreeFn>(ucrt, "free"),
        .aligned_free = exportFunction<FreeFn>(ucrt, "_aligned_free"),
        .set_new_mode = exportFunction<SetNewModeFn>(ucrt, "_set_new_mode"),
        .query_new_mode = exportFunction<QueryNewModeFn>(ucrt, "_query_new_mode"),
        .set_new_handler = exportFunction<SetNewHandlerFn>(ucrt, "_set_new_handler"),
        .query_new_handler = exportFunction<QueryNewHandlerFn>(ucrt, "_query_new_handler"),
    };
    FixtureExports fixture{
        .malloc = exportFunction<FixtureMallocFn>(fixture_module, "sparkAllocationFixtureMalloc"),
        .calloc = exportFunction<FixtureCallocFn>(fixture_module, "sparkAllocationFixtureCalloc"),
        .realloc = exportFunction<FixtureReallocFn>(fixture_module, "sparkAllocationFixtureRealloc"),
        .recalloc = exportFunction<FixtureRecallocFn>(fixture_module, "sparkAllocationFixtureRecalloc"),
        .aligned_malloc = exportFunction<FixtureAlignedMallocFn>(fixture_module, "sparkAllocationFixtureAlignedMalloc"),
        .aligned_realloc =
            exportFunction<FixtureAlignedReallocFn>(fixture_module, "sparkAllocationFixtureAlignedRealloc"),
        .aligned_recalloc =
            exportFunction<FixtureAlignedRecallocFn>(fixture_module, "sparkAllocationFixtureAlignedRecalloc"),
        .aligned_offset_malloc =
            exportFunction<FixtureAlignedOffsetMallocFn>(fixture_module, "sparkAllocationFixtureAlignedOffsetMalloc"),
        .aligned_offset_realloc =
            exportFunction<FixtureAlignedOffsetReallocFn>(fixture_module, "sparkAllocationFixtureAlignedOffsetRealloc"),
        .aligned_offset_recalloc = exportFunction<FixtureAlignedOffsetRecallocFn>(
            fixture_module, "sparkAllocationFixtureAlignedOffsetRecalloc"),
        .malloc_base = exportFunction<FixtureMallocFn>(fixture_module, "sparkAllocationFixtureMallocBase"),
        .calloc_base = exportFunction<FixtureCallocFn>(fixture_module, "sparkAllocationFixtureCallocBase"),
        .realloc_base = exportFunction<FixtureReallocFn>(fixture_module, "sparkAllocationFixtureReallocBase"),
        .free = exportFunction<FixtureFreeFn>(fixture_module, "sparkAllocationFixtureFree"),
        .aligned_free = exportFunction<FixtureAlignedFreeFn>(fixture_module, "sparkAllocationFixtureAlignedFree"),
    };

    const bool exports_ready =
        direct.malloc != nullptr && direct.calloc != nullptr && direct.realloc != nullptr &&
        direct.recalloc != nullptr && direct.aligned_malloc != nullptr && direct.aligned_realloc != nullptr &&
        direct.aligned_recalloc != nullptr && direct.aligned_offset_malloc != nullptr &&
        direct.aligned_offset_realloc != nullptr && direct.aligned_offset_recalloc != nullptr &&
        direct.malloc_base != nullptr && direct.calloc_base != nullptr && direct.realloc_base != nullptr &&
        direct.free != nullptr && direct.aligned_free != nullptr && direct.set_new_mode != nullptr &&
        direct.query_new_mode != nullptr && direct.set_new_handler != nullptr && direct.query_new_handler != nullptr &&
        fixture.malloc != nullptr && fixture.calloc != nullptr && fixture.realloc != nullptr &&
        fixture.recalloc != nullptr && fixture.aligned_malloc != nullptr && fixture.aligned_realloc != nullptr &&
        fixture.aligned_recalloc != nullptr && fixture.aligned_offset_malloc != nullptr &&
        fixture.aligned_offset_realloc != nullptr && fixture.aligned_offset_recalloc != nullptr &&
        fixture.malloc_base != nullptr && fixture.calloc_base != nullptr && fixture.realloc_base != nullptr &&
        fixture.free != nullptr && fixture.aligned_free != nullptr;
    if (!exports_ready) {
        (void)::FreeLibrary(fixture_module);
        return fail("required-export") ? 0 : 1;
    }

    SYSTEM_INFO system_info{};
    ::GetSystemInfo(&system_info);
    const auto max_application_address = reinterpret_cast<std::uintptr_t>(system_info.lpMaximumApplicationAddress);
    if (sizeof(std::size_t) != 8 || kImpossibleSize <= max_application_address ||
        kImpossibleSize + 256 >= kHeapMaxReq) {
        (void)::FreeLibrary(fixture_module);
        return fail("impossible-request-precondition") ? 0 : 1;
    }

    NewHandlerState state{direct, direct.query_new_mode(), direct.query_new_handler()};
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config;
    config.interval_bytes = 1;
    config.session_seed = 0x2468ACE0ULL;
    config.live_only = true;
    std::string error;
    if (!sampler.start(config, error)) {
        (void)::FreeLibrary(fixture_module);
        std::fprintf(stderr, "stage=windows-allocation-new-handler failure=start detail=%s\n", error.c_str());
        return 1;
    }
    const char *gateway_import_failure = nullptr;
    if (!inspectGatewayImports(fixture_module, direct, gateway_import_failure)) {
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture_module);
        std::fprintf(stderr, "stage=windows-allocation-new-handler failure=fixture-gateway-import name=%s\n",
                     gateway_import_failure != nullptr ? gateway_import_failure : "unknown");
        return 1;
    }

    const bool cases_ok =
        runAllocationCase(
            sampler, state, "malloc", [&] { return direct.malloc(kImpossibleSize); },
            [&] { return fixture.malloc(kImpossibleSize); }) &&
        runAllocationCase(
            sampler, state, "calloc", [&] { return direct.calloc(1, kImpossibleSize); },
            [&] { return fixture.calloc(1, kImpossibleSize); }) &&
        runReallocCase(
            sampler, state, "realloc", [&] { return direct.malloc(kSmallSize); },
            [&](void *pointer, std::size_t size) { return direct.realloc(pointer, size); }, direct.free,
            [&] { return fixture.malloc(kSmallSize); },
            [&](void *pointer, std::size_t size) { return fixture.realloc(pointer, size); }, fixture.free) &&
        runReallocCase(
            sampler, state, "recalloc", [&] { return direct.malloc(kSmallSize); },
            [&](void *pointer, std::size_t size) { return direct.recalloc(pointer, 1, size); }, direct.free,
            [&] { return fixture.malloc(kSmallSize); },
            [&](void *pointer, std::size_t size) { return fixture.recalloc(pointer, 1, size); }, fixture.free) &&
        runAllocationCase(
            sampler, state, "aligned_malloc", [&] { return direct.aligned_malloc(kImpossibleSize, kSmallAlignment); },
            [&] { return fixture.aligned_malloc(kImpossibleSize, kSmallAlignment); }) &&
        runReallocCase(
            sampler, state, "aligned_realloc", [&] { return direct.aligned_malloc(kSmallSize, kSmallAlignment); },
            [&](void *pointer, std::size_t size) { return direct.aligned_realloc(pointer, size, kSmallAlignment); },
            direct.aligned_free, [&] { return fixture.aligned_malloc(kSmallSize, kSmallAlignment); },
            [&](void *pointer, std::size_t size) { return fixture.aligned_realloc(pointer, size, kSmallAlignment); },
            fixture.aligned_free) &&
        runReallocCase(
            sampler, state, "aligned_recalloc", [&] { return direct.aligned_malloc(kSmallSize, kSmallAlignment); },
            [&](void *pointer, std::size_t size) { return direct.aligned_recalloc(pointer, 1, size, kSmallAlignment); },
            direct.aligned_free, [&] { return fixture.aligned_malloc(kSmallSize, kSmallAlignment); },
            [&](void *pointer, std::size_t size) {
                return fixture.aligned_recalloc(pointer, 1, size, kSmallAlignment);
            },
            fixture.aligned_free) &&
        runAllocationCase(
            sampler, state, "aligned_offset_malloc",
            [&] { return direct.aligned_offset_malloc(kImpossibleSize, kSmallAlignment, kSmallOffset); },
            [&] { return fixture.aligned_offset_malloc(kImpossibleSize, kSmallAlignment, kSmallOffset); }) &&
        runReallocCase(
            sampler, state, "aligned_offset_realloc",
            [&] { return direct.aligned_offset_malloc(kSmallSize, kSmallAlignment, kSmallOffset); },
            [&](void *pointer, std::size_t size) {
                return direct.aligned_offset_realloc(pointer, size, kSmallAlignment, kSmallOffset);
            },
            direct.aligned_free,
            [&] { return fixture.aligned_offset_malloc(kSmallSize, kSmallAlignment, kSmallOffset); },
            [&](void *pointer, std::size_t size) {
                return fixture.aligned_offset_realloc(pointer, size, kSmallAlignment, kSmallOffset);
            },
            fixture.aligned_free) &&
        runReallocCase(
            sampler, state, "aligned_offset_recalloc",
            [&] { return direct.aligned_offset_malloc(kSmallSize, kSmallAlignment, kSmallOffset); },
            [&](void *pointer, std::size_t size) {
                return direct.aligned_offset_recalloc(pointer, 1, size, kSmallAlignment, kSmallOffset);
            },
            direct.aligned_free,
            [&] { return fixture.aligned_offset_malloc(kSmallSize, kSmallAlignment, kSmallOffset); },
            [&](void *pointer, std::size_t size) {
                return fixture.aligned_offset_recalloc(pointer, 1, size, kSmallAlignment, kSmallOffset);
            },
            fixture.aligned_free) &&
        runAllocationCase(
            sampler, state, "malloc_base", [&] { return direct.malloc_base(kImpossibleSize); },
            [&] { return fixture.malloc_base(kImpossibleSize); }) &&
        runAllocationCase(
            sampler, state, "calloc_base", [&] { return direct.calloc_base(1, kImpossibleSize); },
            [&] { return fixture.calloc_base(1, kImpossibleSize); }) &&
        runReallocCase(
            sampler, state, "realloc_base", [&] { return direct.malloc_base(kSmallSize); },
            [&](void *pointer, std::size_t size) { return direct.realloc_base(pointer, size); }, direct.free,
            [&] { return fixture.malloc_base(kSmallSize); },
            [&](void *pointer, std::size_t size) { return fixture.realloc_base(pointer, size); }, fixture.free);

    state.restore();
    if (!cases_ok) {
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture_module);
        return 1;
    }

    void *subsequent = fixture.malloc(kSmallSize);
    if (subsequent == nullptr) {
        (void)sampler.shutdown(error);
        (void)::FreeLibrary(fixture_module);
        return fail("subsequent-tracked-allocation") ? 0 : 1;
    }
    fixture.free(subsequent);
    if (!sampler.stop(error) || !error.empty() || !sampler.shutdown(error) || !error.empty()) {
        (void)::FreeLibrary(fixture_module);
        return fail("sampler-cleanup") ? 0 : 1;
    }
    if (!::FreeLibrary(fixture_module)) {
        return fail("fixture-unload") ? 0 : 1;
    }
    std::fprintf(stderr, "stage=windows-allocation-new-handler pass\n");
    return 0;
}
