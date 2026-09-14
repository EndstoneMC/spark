#include "native/alloc/windows_permanent_iat_gateway.h"

#ifndef _WIN32
#error "windows_permanent_iat_gateway.cpp must only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>

namespace spark::permanent_iat_gateway {
namespace {

constexpr std::uint64_t KGatewayMagic = 0x3154414947504B53ULL;  // "SKPGIAT1" marker.
constexpr std::uint32_t KGatewayAbiVersion = 3;
constexpr std::size_t KGatewayCodeCapacity = 256;
constexpr std::size_t KGatewayImageCapacity = 512;
constexpr std::size_t KGatewayAllocationSize = 4096;
constexpr std::size_t KGatewayUnwindInfoSize = 12;
constexpr std::uint64_t KGateClosed = 0;
constexpr std::uint64_t KGateOpen = 1;
constexpr std::uint32_t KMaxStackArguments = 1;

struct GatewayState {
    std::uint64_t magic = KGatewayMagic;
    std::uint32_t abi_version = KGatewayAbiVersion;
    std::uint32_t struct_size = sizeof(GatewayState);
    std::atomic<std::uint64_t> generation{1};
    std::atomic<std::uint64_t> gate{KGateClosed};
    std::atomic<std::uint64_t> active{0};
    std::atomic<void *> handler{nullptr};
    void *original = nullptr;
    void *gateway = nullptr;
    std::uint64_t code_hash = 0;
    std::uint32_t code_size = 0;
    std::uint32_t stack_argument_count = 0;
    std::uint32_t call_stub_offset = 0;
    std::uint32_t unwind_info_offset = 0;
    RUNTIME_FUNCTION runtime_function{};
};

struct GatewayImageLayout {
    std::size_t code_size = 0;
    std::size_t call_stub_offset = 0;
    std::size_t cleanup_handler_offset = 0;
    std::size_t unwind_info_offset = 0;
    std::size_t image_size = 0;
    RUNTIME_FUNCTION runtime_function{};
};

static_assert(offsetof(GatewayState, generation) == 16);
static_assert(offsetof(GatewayState, gate) == 24);
static_assert(offsetof(GatewayState, active) == 32);
static_assert(offsetof(GatewayState, handler) == 40);
static_assert(offsetof(GatewayState, original) == 48);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<void *>::is_always_lock_free);

[[nodiscard]] constexpr std::size_t alignUp(std::size_t value, std::size_t alignment) noexcept
{
    return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] std::uint64_t hashBytes(const void *data, std::size_t size) noexcept
{
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t value = offset;
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    for (std::size_t index = 0; index < size; ++index) {
        value ^= bytes[index];
        value *= prime;
    }
    return value;
}

[[nodiscard]] bool mitigationAllowsPrototype(std::string &error) noexcept
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamic_code{};
    if (::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &dynamic_code,
                                     static_cast<SIZE_T>(sizeof(dynamic_code))) == FALSE) {
        error = "GetProcessMitigationPolicy(ProcessDynamicCodePolicy) failed: " + std::to_string(::GetLastError());
        return false;
    }
    if (dynamic_code.ProhibitDynamicCode != 0) {
        error = "ProcessDynamicCodePolicy prohibits permanent IAT gateway executable code";
        return false;
    }

    // CFG does not by itself prohibit this gateway. Windows marks committed executable
    // pages as valid CFG call targets by default unless PAGE_TARGETS_INVALID or
    // PAGE_TARGETS_NO_UPDATE is requested. The gateway is published only after its
    // RW -> RX transition, and the CFG-enabled stress job exercises both indirect
    // entry into the generated page and its indirect dispatch to handler/original.
    return true;
}

[[nodiscard]] bool emit(std::array<std::uint8_t, KGatewayImageCapacity> &image, std::size_t &out,
                        std::initializer_list<std::uint8_t> bytes, std::string &error)
{
    if (out + bytes.size() > KGatewayCodeCapacity) {
        error = "permanent IAT gateway machine-code buffer capacity exceeded";
        return false;
    }
    for (const std::uint8_t byte : bytes) {
        image[out++] = byte;
    }
    return true;
}

[[nodiscard]] bool patchRel8(std::array<std::uint8_t, KGatewayImageCapacity> &image, std::size_t branch_offset,
                             std::size_t target, std::string &error)
{
    if (branch_offset + 2 > KGatewayCodeCapacity || target > KGatewayCodeCapacity) {
        error = "permanent IAT gateway branch patch is outside code buffer";
        return false;
    }
    const std::intptr_t delta = static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(branch_offset + 2);
    if (delta < -128 || delta > 127) {
        error = "permanent IAT gateway branch exceeds rel8 range";
        return false;
    }
    image[branch_offset + 1] = static_cast<std::uint8_t>(static_cast<std::int8_t>(delta));
    return true;
}

[[nodiscard]] bool sameRuntimeFunction(const RUNTIME_FUNCTION &left, const RUNTIME_FUNCTION &right) noexcept
{
    return left.BeginAddress == right.BeginAddress && left.EndAddress == right.EndAddress &&
           left.UnwindData == right.UnwindData;
}

[[nodiscard]] bool buildGatewayImage(GatewayState *state, std::array<std::uint8_t, KGatewayImageCapacity> &image,
                                     GatewayImageLayout &layout, std::string &error)
{
    image = {};
    layout = {};
    if (state == nullptr || state->stack_argument_count > KMaxStackArguments) {
        error = "unsupported permanent IAT gateway stack-argument count";
        return false;
    }

    // The entry/admission block is a true leaf: it never changes RSP and reaches
    // the original allocator via a tail jump. Only the dedicated call stub below
    // owns a stack frame. Keeping these ranges separate lets the Windows x64
    // unwinder treat entry/fallback as leaf code and apply one precise dynamic
    // RUNTIME_FUNCTION to the call stub.
    std::size_t code_size = 0;
    const auto state_address = reinterpret_cast<std::uint64_t>(state);
    if (!emit(image, code_size, {0x49, 0xBB}, error)) {  // mov r11,state
        return false;
    }
    if (code_size + sizeof(state_address) > KGatewayCodeCapacity) {
        error = "permanent IAT gateway state immediate exceeds code buffer";
        return false;
    }
    std::memcpy(image.data() + code_size, &state_address, sizeof(state_address));
    code_size += sizeof(state_address);

    if (!emit(image, code_size, {0x49, 0x83, 0x7B, 0x18, 0x00}, error)) {  // cmp [r11+gate],0
        return false;
    }
    const std::size_t initial_fallback = code_size;
    if (!emit(image, code_size, {0x74, 0x00}, error) ||                    // je fallback
        !emit(image, code_size, {0x4D, 0x8B, 0x53, 0x10}, error) ||        // mov r10,[r11+generation]
        !emit(image, code_size, {0xF0, 0x49, 0xFF, 0x43, 0x20}, error) ||  // lock inc [r11+active]
        !emit(image, code_size, {0x49, 0x83, 0x7B, 0x18, 0x00}, error)) {  // cmp [r11+gate],0
        return false;
    }
    const std::size_t closed_rollback = code_size;
    if (!emit(image, code_size, {0x74, 0x00}, error) ||              // je rollback
        !emit(image, code_size, {0x4D, 0x3B, 0x53, 0x10}, error)) {  // cmp r10,[r11+generation]
        return false;
    }
    const std::size_t generation_rollback = code_size;
    if (!emit(image, code_size, {0x75, 0x00}, error) ||              // jne rollback
        !emit(image, code_size, {0x4D, 0x8B, 0x53, 0x28}, error) ||  // mov r10,[r11+handler]
        !emit(image, code_size, {0x4D, 0x85, 0xD2}, error)) {        // test r10,r10
        return false;
    }
    const std::size_t null_rollback = code_size;
    if (!emit(image, code_size, {0x74, 0x00}, error)) {  // je rollback
        return false;
    }

    // Preserve the fifth Windows x64 argument before the call stub allocates its
    // own 32-byte home area. RAX is volatile and does not carry an allocator input.
    if (state->stack_argument_count == 1 &&
        !emit(image, code_size, {0x48, 0x8B, 0x44, 0x24, 0x28}, error)) {  // mov rax,[rsp+40]
        return false;
    }
    const std::size_t call_stub_jump = code_size;
    if (!emit(image, code_size, {0xEB, 0x00}, error)) {  // jmp call_stub
        return false;
    }

    const std::size_t rollback = code_size;
    if (!emit(image, code_size, {0xF0, 0x49, 0xFF, 0x4B, 0x20}, error)) {  // lock dec [r11+active]
        return false;
    }
    const std::size_t fallback = code_size;
    if (!emit(image, code_size, {0x4D, 0x8B, 0x53, 0x30}, error) ||  // mov r10,[r11+original]
        !emit(image, code_size, {0x41, 0xFF, 0xE2}, error)) {        // jmp r10
        return false;
    }

    // The cleanup handler is a true leaf and lives outside the registered
    // function range. The admission jump lands after it at the call stub.
    const std::size_t cleanup_handler = code_size;
    if (!emit(image, code_size, {0x8B, 0x41, 0x04}, error) ||                    // mov eax,[rcx+4]
        !emit(image, code_size, {0xF7, 0xC0, 0x06, 0x00, 0x00, 0x00}, error)) {  // test eax,UNWINDING|EXIT_UNWIND
        return false;
    }
    const std::size_t handler_not_unwinding = code_size;
    if (!emit(image, code_size, {0x74, 0x00}, error) ||                          // je return
        !emit(image, code_size, {0xF7, 0xC0, 0x20, 0x00, 0x00, 0x00}, error)) {  // test eax,TARGET_UNWIND
        return false;
    }
    const std::size_t handler_target_unwind = code_size;
    if (!emit(image, code_size, {0x75, 0x00}, error) ||              // jne return
        !emit(image, code_size, {0x31, 0xC0}, error) ||              // xor eax,eax
        !emit(image, code_size, {0x48, 0x87, 0x42, 0x28}, error) ||  // xchg [rdx+40],rax
        !emit(image, code_size, {0x48, 0x85, 0xC0}, error)) {        // test rax,rax
        return false;
    }
    const std::size_t handler_token_consumed = code_size;
    if (!emit(image, code_size, {0x74, 0x00}, error) ||                    // je return
        !emit(image, code_size, {0x4C, 0x8B, 0x5A, 0x30}, error) ||        // mov r11,[rdx+48]
        !emit(image, code_size, {0xF0, 0x49, 0xFF, 0x4B, 0x20}, error)) {  // lock dec [r11+active]
        return false;
    }
    const std::size_t handler_return = code_size;
    if (!emit(image, code_size, {0xB8, 0x01, 0x00, 0x00, 0x00}, error) ||  // return ExceptionContinueSearch
        !emit(image, code_size, {0xC3}, error)) {
        return false;
    }
    if (!patchRel8(image, handler_not_unwinding, handler_return, error) ||
        !patchRel8(image, handler_target_unwind, handler_return, error) ||
        !patchRel8(image, handler_token_consumed, handler_return, error)) {
        return false;
    }

    // This is the only non-leaf range. Its four-byte stack-allocation prologue
    // is described by the dynamic UNWIND_INFO emitted after the machine code.
    const std::size_t call_stub = code_size;
    if (!emit(image, code_size, {0x48, 0x83, 0xEC, 0x38}, error)) {  // sub rsp,56
        return false;
    }
    if (state->stack_argument_count == 1 &&
        !emit(image, code_size, {0x48, 0x89, 0x44, 0x24, 0x20}, error)) {  // mov [rsp+32],rax
        return false;
    }
    if (!emit(image, code_size, {0x48, 0xC7, 0x44, 0x24, 0x28, 0x01, 0x00, 0x00, 0x00}, error) ||
        !emit(image, code_size, {0x4C, 0x89, 0x5C, 0x24, 0x30}, error)) {  // token=1, state=[rsp+48]
        return false;
    }
    if (!emit(image, code_size, {0x41, 0xFF, 0xD2}, error) ||  // call r10
        !emit(image, code_size, {0x48, 0xC7, 0x44, 0x24, 0x28, 0x00, 0x00, 0x00, 0x00}, error) ||
        !emit(image, code_size, {0x4C, 0x8B, 0x5C, 0x24, 0x30}, error)) {  // clear token, reload state
        return false;
    }
    if (!emit(image, code_size, {0xF0, 0x49, 0xFF, 0x4B, 0x20}, error) ||  // lock dec [r11+active]
        !emit(image, code_size, {0x48, 0x83, 0xC4, 0x38}, error) ||        // add rsp,56
        !emit(image, code_size, {0xC3}, error)) {                          // ret original caller
        return false;
    }

    if (!patchRel8(image, initial_fallback, fallback, error) || !patchRel8(image, closed_rollback, rollback, error) ||
        !patchRel8(image, generation_rollback, rollback, error) || !patchRel8(image, null_rollback, rollback, error) ||
        !patchRel8(image, call_stub_jump, call_stub, error)) {
        return false;
    }

    const std::size_t unwind_info = alignUp(code_size, 4);
    if (unwind_info + KGatewayUnwindInfoSize > image.size()) {
        error = "permanent IAT gateway unwind metadata exceeds image capacity";
        return false;
    }

    // UNWIND_INFO version=1, UHANDLER, prologue=4, one unwind code,
    // no frame register. UWOP_ALLOC_SMALL with OpInfo=6 represents 56 bytes.
    const std::array<std::uint8_t, 8> unwind_header{0x11, 0x04, 0x01, 0x00, 0x04, 0x62, 0x00, 0x00};
    std::memcpy(image.data() + unwind_info, unwind_header.data(), unwind_header.size());
    const auto cleanup_handler_rva = static_cast<DWORD>(cleanup_handler);
    std::memcpy(image.data() + unwind_info + unwind_header.size(), &cleanup_handler_rva, sizeof(cleanup_handler_rva));

    if (call_stub > std::numeric_limits<DWORD>::max() || code_size > std::numeric_limits<DWORD>::max() ||
        unwind_info > std::numeric_limits<DWORD>::max()) {
        error = "permanent IAT gateway unwind RVA exceeds x64 runtime-function range";
        return false;
    }

    layout.code_size = code_size;
    layout.call_stub_offset = call_stub;
    layout.cleanup_handler_offset = cleanup_handler;
    layout.unwind_info_offset = unwind_info;
    layout.image_size = unwind_info + KGatewayUnwindInfoSize;
    layout.runtime_function.BeginAddress = static_cast<DWORD>(call_stub);
    layout.runtime_function.EndAddress = static_cast<DWORD>(code_size);
    layout.runtime_function.UnwindData = static_cast<DWORD>(unwind_info);
    return true;
}

[[nodiscard]] bool isExecutableReadOnly(DWORD protection) noexcept
{
    const DWORD base = protection & 0xFFU;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ;
}

[[nodiscard]] bool isWritableNonExecutable(DWORD protection) noexcept
{
    const DWORD base = protection & 0xFFU;
    return base == PAGE_READWRITE || base == PAGE_WRITECOPY;
}

[[nodiscard]] bool queryCommitted(void *address, MEMORY_BASIC_INFORMATION &memory, std::string &error)
{
    memory = {};
    if (address == nullptr || ::VirtualQuery(address, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
        memory.BaseAddress == nullptr || memory.RegionSize == 0) {
        error = "permanent IAT gateway address is not committed memory";
        return false;
    }
    return true;
}

[[nodiscard]] GatewayState *stateFromHandle(const PermanentIatGatewayHandle &handle) noexcept
{
    return static_cast<GatewayState *>(handle.state);
}

[[nodiscard]] bool validateStateIdentity(GatewayState *state, void *gateway, std::string &error)
{
    if (state == nullptr || state->magic != KGatewayMagic || state->abi_version != KGatewayAbiVersion ||
        state->struct_size != sizeof(GatewayState) || state->gateway != gateway || state->original == nullptr ||
        state->stack_argument_count > KMaxStackArguments || state->code_size == 0 ||
        state->code_size > KGatewayCodeCapacity || state->call_stub_offset >= state->code_size ||
        state->unwind_info_offset < state->code_size ||
        static_cast<std::size_t>(state->unwind_info_offset) + KGatewayUnwindInfoSize > KGatewayAllocationSize ||
        state->runtime_function.BeginAddress != state->call_stub_offset ||
        state->runtime_function.EndAddress != state->code_size ||
        state->runtime_function.UnwindData != state->unwind_info_offset) {
        error = "permanent IAT gateway state identity/ABI validation failed";
        return false;
    }
    return true;
}

[[nodiscard]] bool validateUnwindRegistration(GatewayState *state, std::string &error) noexcept
{
    if (state == nullptr || state->gateway == nullptr) {
        error = "permanent IAT gateway unwind validation received null state";
        return false;
    }
    const auto gateway_base = reinterpret_cast<DWORD64>(state->gateway);
    const DWORD64 control_pc = gateway_base + state->call_stub_offset + 4;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION found = ::RtlLookupFunctionEntry(control_pc, &image_base, nullptr);
    if (found == nullptr || image_base != gateway_base || !sameRuntimeFunction(*found, state->runtime_function)) {
        error = "permanent IAT gateway dynamic unwind table lookup failed";
        return false;
    }
    return true;
}

[[nodiscard]] bool waitForZero(GatewayState *state, std::uint64_t timeout_ms, std::string &error)
{
    const std::uint64_t deadline = ::GetTickCount64() + timeout_ms;
    while (state->active.load(std::memory_order_acquire) != 0) {
        if (::GetTickCount64() >= deadline) {
            error = "timed out draining permanent IAT gateway admitted callbacks";
            return false;
        }
        ::Sleep(0);
    }
    return true;
}

void populateHandle(GatewayState *state, const MEMORY_BASIC_INFORMATION &code_memory,
                    const MEMORY_BASIC_INFORMATION &state_memory, PermanentIatGatewayHandle &handle) noexcept
{
    handle.gateway = state->gateway;
    handle.original = state->original;
    handle.state = state;
    handle.permanent_rx_bytes = code_memory.RegionSize;
    handle.permanent_rw_bytes = state_memory.RegionSize;
    handle.generation = state->generation.load(std::memory_order_acquire);
    handle.stack_argument_count = state->stack_argument_count;
}

}  // namespace

bool createPermanentIatGateway(void *original, std::uint32_t stack_argument_count, PermanentIatGatewayHandle &handle,
                               std::string &error)
{
    handle = {};
    error.clear();
    if (original == nullptr) {
        error = "permanent IAT gateway original is null";
        return false;
    }
    if (stack_argument_count > KMaxStackArguments) {
        error = "permanent IAT gateway prototype supports at most one stack argument";
        return false;
    }
    if (!mitigationAllowsPrototype(error)) {
        return false;
    }

    MEMORY_BASIC_INFORMATION original_memory{};
    if (!queryCommitted(original, original_memory, error) || !isExecutableReadOnly(original_memory.Protect)) {
        error = "permanent IAT gateway original is not committed executable read-only memory";
        return false;
    }

    void *state_memory_raw = ::VirtualAlloc(nullptr, sizeof(GatewayState), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (state_memory_raw == nullptr) {
        error = "VirtualAlloc permanent IAT gateway state failed: " + std::to_string(::GetLastError());
        return false;
    }
    void *code_memory_raw = ::VirtualAlloc(nullptr, KGatewayAllocationSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (code_memory_raw == nullptr) {
        const DWORD failure = ::GetLastError();
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        error = "VirtualAlloc permanent IAT gateway code failed: " + std::to_string(failure);
        return false;
    }

    auto *state = ::new (state_memory_raw) GatewayState{};
    state->original = original;
    state->gateway = code_memory_raw;
    state->stack_argument_count = stack_argument_count;

    std::array<std::uint8_t, KGatewayImageCapacity> image{};
    GatewayImageLayout layout;
    if (!buildGatewayImage(state, image, layout, error)) {
        ::VirtualFree(code_memory_raw, 0, MEM_RELEASE);
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        return false;
    }
    state->code_size = static_cast<std::uint32_t>(layout.code_size);
    state->call_stub_offset = static_cast<std::uint32_t>(layout.call_stub_offset);
    state->unwind_info_offset = static_cast<std::uint32_t>(layout.unwind_info_offset);
    state->runtime_function = layout.runtime_function;
    std::memcpy(code_memory_raw, image.data(), KGatewayImageCapacity);

    DWORD old_code_protection = 0;
    if (::VirtualProtect(code_memory_raw, KGatewayAllocationSize, PAGE_EXECUTE_READ, &old_code_protection) == FALSE) {
        const DWORD failure = ::GetLastError();
        ::VirtualFree(code_memory_raw, 0, MEM_RELEASE);
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        error = "VirtualProtect permanent IAT gateway RX failed: " + std::to_string(failure);
        return false;
    }
    if (::FlushInstructionCache(::GetCurrentProcess(), code_memory_raw, KGatewayAllocationSize) == FALSE) {
        const DWORD failure = ::GetLastError();
        ::VirtualFree(code_memory_raw, 0, MEM_RELEASE);
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        error = "FlushInstructionCache permanent IAT gateway failed: " + std::to_string(failure);
        return false;
    }
    state->code_hash = hashBytes(code_memory_raw, KGatewayImageCapacity);

    MEMORY_BASIC_INFORMATION code_memory{};
    MEMORY_BASIC_INFORMATION state_memory{};
    if (!queryCommitted(code_memory_raw, code_memory, error) || !isExecutableReadOnly(code_memory.Protect) ||
        !queryCommitted(state, state_memory, error) || !isWritableNonExecutable(state_memory.Protect)) {
        ::VirtualFree(code_memory_raw, 0, MEM_RELEASE);
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        error = "permanent IAT gateway memory protections violate W^X invariant before publication";
        return false;
    }

    // The runtime-function entry and its UNWIND_INFO are process-lifetime just
    // like the gateway code/state. No successful gateway calls
    // RtlDeleteFunctionTable: doing so on plugin unload would recreate the same
    // stale-executable/unwind-metadata hazard this architecture is designed to
    // eliminate.
    if (::RtlAddFunctionTable(&state->runtime_function, 1, reinterpret_cast<DWORD64>(code_memory_raw)) == FALSE) {
        ::VirtualFree(code_memory_raw, 0, MEM_RELEASE);
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        error = "RtlAddFunctionTable permanent IAT gateway failed";
        return false;
    }
    if (!validateUnwindRegistration(state, error)) {
        // A registered function table and the generated image are process
        // lifetime. Never remove a successful registration while code can
        // still be reachable through a published IAT slot.
        return false;
    }

    // This is the lifetime boundary for successful construction. The caller may
    // publish gateway into arbitrary process IAT slots immediately after return,
    // so successful code/state/unwind metadata is process-lifetime and has no
    // destroy API.
    populateHandle(state, code_memory, state_memory, handle);
    return true;
}

bool discoverPermanentIatGateway(void *gateway, PermanentIatGatewayHandle &handle, std::string &error)
{
    handle = {};
    error.clear();
    if (gateway == nullptr) {
        error = "permanent IAT gateway discovery pointer is null";
        return false;
    }

    MEMORY_BASIC_INFORMATION code_memory{};
    if (!queryCommitted(gateway, code_memory, error) || !isExecutableReadOnly(code_memory.Protect)) {
        error = "permanent IAT gateway discovery pointer is not committed RX memory";
        return false;
    }
    const auto gateway_value = reinterpret_cast<std::uintptr_t>(gateway);
    const auto code_region_end = reinterpret_cast<std::uintptr_t>(code_memory.BaseAddress) + code_memory.RegionSize;
    auto *code = static_cast<std::uint8_t *>(gateway);
    if (gateway_value > code_region_end || code_region_end - gateway_value < 10 || code[0] != 0x49 || code[1] != 0xBB) {
        error = "permanent IAT gateway does not contain the expected state signature";
        return false;
    }

    GatewayState *state = nullptr;
    static_assert(sizeof(GatewayState *) == sizeof(std::uint64_t));
    std::memcpy(static_cast<void *>(&state), code + 2, sizeof(GatewayState *));
    MEMORY_BASIC_INFORMATION state_memory{};
    if (!queryCommitted(state, state_memory, error) || !isWritableNonExecutable(state_memory.Protect)) {
        error = "decoded permanent IAT gateway state is not committed non-executable writable memory";
        return false;
    }
    if (!validateStateIdentity(state, gateway, error)) {
        return false;
    }

    std::array<std::uint8_t, KGatewayImageCapacity> expected_image{};
    GatewayImageLayout expected_layout;
    if (!buildGatewayImage(state, expected_image, expected_layout, error) ||
        expected_layout.code_size != state->code_size || expected_layout.call_stub_offset != state->call_stub_offset ||
        expected_layout.unwind_info_offset != state->unwind_info_offset ||
        !sameRuntimeFunction(expected_layout.runtime_function, state->runtime_function) ||
        std::memcmp(gateway, expected_image.data(), KGatewayImageCapacity) != 0 ||
        state->code_hash != hashBytes(gateway, KGatewayImageCapacity)) {
        if (error.empty()) {
            error = "permanent IAT gateway code/unwind signature validation failed";
        }
        return false;
    }
    if (!validateUnwindRegistration(state, error)) {
        return false;
    }

    MEMORY_BASIC_INFORMATION original_memory{};
    if (!queryCommitted(state->original, original_memory, error) || !isExecutableReadOnly(original_memory.Protect)) {
        error = "permanent IAT gateway original is no longer committed RX memory";
        return false;
    }

    populateHandle(state, code_memory, state_memory, handle);
    return true;
}

bool bindPermanentIatGateway(PermanentIatGatewayHandle &handle, void *handler, std::uint64_t timeout_ms,
                             std::string &error)
{
    error.clear();
    auto *state = stateFromHandle(handle);
    if (state == nullptr || handler == nullptr || !validateStateIdentity(state, handle.gateway, error)) {
        if (error.empty()) {
            error = "permanent IAT gateway bind received a null state or handler";
        }
        return false;
    }

    MEMORY_BASIC_INFORMATION handler_memory{};
    if (!queryCommitted(handler, handler_memory, error) || !isExecutableReadOnly(handler_memory.Protect)) {
        error = "permanent IAT gateway handler is not committed executable read-only memory";
        return false;
    }
    if (state->gate.load(std::memory_order_acquire) != KGateClosed ||
        state->handler.load(std::memory_order_acquire) != nullptr) {
        error = "permanent IAT gateway bind requires a detached state";
        return false;
    }
    if (!waitForZero(state, timeout_ms, error)) {
        return false;
    }

    const std::uint64_t current = state->generation.load(std::memory_order_acquire);
    if (current >= std::numeric_limits<std::uint64_t>::max() - 1) {
        error = "permanent IAT gateway generation exhausted; admission remains closed";
        return false;
    }
    const std::uint64_t generation = state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    state->handler.store(handler, std::memory_order_release);
    state->gate.store(KGateOpen, std::memory_order_seq_cst);
    handle.generation = generation;
    return true;
}

bool detachPermanentIatGateway(PermanentIatGatewayHandle &handle, std::uint64_t timeout_ms, std::string &error)
{
    error.clear();
    auto *state = stateFromHandle(handle);
    if (state == nullptr || !validateStateIdentity(state, handle.gateway, error)) {
        if (error.empty()) {
            error = "permanent IAT gateway detach received a null state";
        }
        return false;
    }

    state->gate.store(KGateClosed, std::memory_order_seq_cst);
    const std::uint64_t current = state->generation.load(std::memory_order_acquire);
    if (current == std::numeric_limits<std::uint64_t>::max()) {
        error = "permanent IAT gateway generation exhausted after admission close";
        return false;
    }
    const std::uint64_t generation = state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    handle.generation = generation;

    if (!waitForZero(state, timeout_ms, error)) {
        return false;
    }
    state->handler.store(nullptr, std::memory_order_seq_cst);
    return true;
}

void *permanentIatGatewayOriginal(const PermanentIatGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->original : nullptr;
}

void *permanentIatGatewayHandler(const PermanentIatGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->handler.load(std::memory_order_acquire) : nullptr;
}

std::uint64_t permanentIatGatewayActive(const PermanentIatGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->active.load(std::memory_order_acquire) : 0;
}

std::uint64_t permanentIatGatewayGeneration(const PermanentIatGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->generation.load(std::memory_order_acquire) : 0;
}

bool permanentIatGatewayAdmissionOpen(const PermanentIatGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr && state->gate.load(std::memory_order_acquire) == KGateOpen;
}

}  // namespace spark::permanent_iat_gateway
