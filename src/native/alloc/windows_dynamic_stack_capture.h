#pragma once

#ifndef _WIN32
#error "windows_dynamic_stack_capture.h must only be included on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace spark {
namespace dynamic_stack_capture_detail {

constexpr std::uint64_t kPermanentIatGatewayMagic = 0x3154414947504B53ULL;  // "SKPGIAT1".
constexpr std::uint32_t kPermanentIatGatewayAbiVersion = 3;
constexpr std::size_t kPermanentIatGatewayCodeCapacity = 256;
constexpr std::size_t kGatewayStateAbiOffset = 8;
constexpr std::size_t kGatewayStateGatewayOffset = 56;
constexpr std::size_t kGatewayStateBytesNeeded = kGatewayStateGatewayOffset + sizeof(void *);
constexpr ULONG kMaximumWalkSteps = 256;

[[nodiscard]] inline bool readableRange(std::uintptr_t address, std::size_t bytes) noexcept
{
    if (address == 0 || bytes == 0 || address > (std::numeric_limits<std::uintptr_t>::max)() - bytes) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (::VirtualQuery(reinterpret_cast<const void *>(address), &memory, sizeof(memory)) == 0 ||
        memory.State != MEM_COMMIT || memory.BaseAddress == nullptr || (memory.Protect & PAGE_GUARD) != 0 ||
        (memory.Protect & 0xFFU) == PAGE_NOACCESS) {
        return false;
    }
    const auto region_begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto region_end = region_begin + memory.RegionSize;
    return address >= region_begin && address + bytes <= region_end;
}

[[nodiscard]] inline bool permanentIatGatewayFrame(DWORD64 control_pc, DWORD64 image_base,
                                                   const RUNTIME_FUNCTION *function) noexcept
{
    if (control_pc == 0 || image_base == 0 || function == nullptr ||
        function->BeginAddress >= kPermanentIatGatewayCodeCapacity ||
        function->EndAddress > kPermanentIatGatewayCodeCapacity || control_pc < image_base + function->BeginAddress ||
        control_pc >= image_base + function->EndAddress ||
        !readableRange(static_cast<std::uintptr_t>(image_base), 10)) {
        return false;
    }

    const auto *code = reinterpret_cast<const std::uint8_t *>(static_cast<std::uintptr_t>(image_base));
    if (code[0] != 0x49 || code[1] != 0xBB) {  // mov r11, <GatewayState *>
        return false;
    }

    std::uint64_t state_value = 0;
    std::memcpy(&state_value, code + 2, sizeof(state_value));
    if (!readableRange(static_cast<std::uintptr_t>(state_value), kGatewayStateBytesNeeded)) {
        return false;
    }

    const auto *state = reinterpret_cast<const std::uint8_t *>(static_cast<std::uintptr_t>(state_value));
    std::uint64_t magic = 0;
    std::uint32_t abi_version = 0;
    std::uint64_t gateway_value = 0;
    std::memcpy(&magic, state, sizeof(magic));
    std::memcpy(&abi_version, state + kGatewayStateAbiOffset, sizeof(abi_version));
    std::memcpy(&gateway_value, state + kGatewayStateGatewayOffset, sizeof(gateway_value));
    return magic == kPermanentIatGatewayMagic && abi_version == kPermanentIatGatewayAbiVersion &&
           gateway_value == image_base;
}

[[nodiscard]] inline bool currentStackBounds(std::uintptr_t &low, std::uintptr_t &high) noexcept
{
    PTEB teb = ::NtCurrentTeb();
    if (teb == nullptr) {
        return false;
    }
    // NT_TIB is the architectural prefix of TEB on Windows. Recent public SDK
    // definitions intentionally hide the NtTib field on _TEB, so inspect the
    // documented prefix layout rather than depending on that private member.
    const auto *tib = reinterpret_cast<const NT_TIB *>(teb);
    low = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
    high = reinterpret_cast<std::uintptr_t>(tib->StackBase);
    return low != 0 && high > low;
}

[[nodiscard]] inline bool popLeafFrame(CONTEXT &context, std::uintptr_t stack_low, std::uintptr_t stack_high) noexcept
{
    const auto rsp = static_cast<std::uintptr_t>(context.Rsp);
    if (rsp < stack_low || rsp > stack_high || stack_high - rsp < sizeof(DWORD64)) {
        return false;
    }
    DWORD64 return_address = 0;
    std::memcpy(&return_address, reinterpret_cast<const void *>(rsp), sizeof(return_address));
    context.Rip = return_address;
    context.Rsp += sizeof(DWORD64);
    return true;
}

}  // namespace dynamic_stack_capture_detail

// RtlCaptureStackBackTrace's fast walker does not reliably consult dynamic
// RtlAddFunctionTable registrations. The permanent-IAT backend necessarily has
// process-lifetime generated code, so allocation stack capture uses the normal
// x64 unwind primitives directly. Instrumentation frames from the permanent
// gateway itself are consumed for unwind correctness but are not exported.
[[nodiscard]] inline USHORT WINAPI captureDynamicAwareStackBackTrace(ULONG frames_to_skip, ULONG frames_to_capture,
                                                                     PVOID *back_trace, PULONG back_trace_hash) noexcept
{
    if (back_trace == nullptr || frames_to_capture == 0) {
        if (back_trace_hash != nullptr) {
            *back_trace_hash = 0;
        }
        return 0;
    }

    CONTEXT context{};
    ::RtlCaptureContext(&context);
    UNWIND_HISTORY_TABLE history{};
    std::uintptr_t stack_low = 0;
    std::uintptr_t stack_high = 0;
    if (!dynamic_stack_capture_detail::currentStackBounds(stack_low, stack_high)) {
        if (back_trace_hash != nullptr) {
            *back_trace_hash = 0;
        }
        return 0;
    }

    ULONG skipped = 0;
    USHORT captured = 0;
    ULONG hash = 0;
    bool candidate_frame = false;
    for (ULONG step = 0; step < dynamic_stack_capture_detail::kMaximumWalkSteps; ++step) {
        const DWORD64 previous_rip = context.Rip;
        const DWORD64 previous_rsp = context.Rsp;
        if (previous_rip == 0 || previous_rsp == 0) {
            break;
        }

        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = ::RtlLookupFunctionEntry(context.Rip, &image_base, &history);

        // The previous iteration already unwound to this frame. Classify and
        // emit it before unwinding again so each stack level needs only one
        // RtlLookupFunctionEntry call. The first captured CONTEXT is internal
        // to this helper and therefore is never emitted.
        if (candidate_frame &&
            !dynamic_stack_capture_detail::permanentIatGatewayFrame(context.Rip, image_base, function)) {
            if (skipped < frames_to_skip) {
                ++skipped;
            }
            else {
                back_trace[captured++] = reinterpret_cast<void *>(static_cast<std::uintptr_t>(context.Rip));
                hash += static_cast<ULONG>(context.Rip);
                if (captured >= frames_to_capture) {
                    break;
                }
            }
        }

        if (function == nullptr) {
            if (!dynamic_stack_capture_detail::popLeafFrame(context, stack_low, stack_high)) {
                break;
            }
        }
        else {
            PVOID handler_data = nullptr;
            DWORD64 establisher_frame = 0;
            (void)::RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                                     &establisher_frame, nullptr);
        }

        if (context.Rip == 0 || context.Rsp == 0 || (context.Rip == previous_rip && context.Rsp == previous_rsp)) {
            break;
        }
        if (context.Rsp < stack_low || context.Rsp > stack_high) {
            break;
        }
        candidate_frame = true;
    }

    if (back_trace_hash != nullptr) {
        *back_trace_hash = hash;
    }
    return captured;
}

}  // namespace spark
