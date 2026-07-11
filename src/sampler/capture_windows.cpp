#include "sampler/capture.h"

// Windows stack capture: there are no POSIX signals, so we cannot run code on the
// target thread at a sampling instant. Instead the sampler thread suspends the target,
// reads its CONTEXT, and walks the stack with StackWalk64, then resumes it. The raw
// addresses are resolved later by cpptrace (via the PDB), exactly as on Linux.
//
// NOTE: this backend is implemented but has not been exercised on a real Windows BDS
// in this environment.

#include <atomic>

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

namespace spark {

namespace {
std::atomic<bool> g_armed{false};
}  // namespace

bool Capture::arm()
{
    if (g_armed.load()) {
        return true;
    }
    SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);  // needed by StackWalk64's module callbacks
    g_armed.store(true);
    return true;
}

void Capture::disarm()
{
    if (!g_armed.exchange(false)) {
        return;
    }
    SymCleanup(GetCurrentProcess());
}

bool Capture::captureThread(std::uint64_t tid, CaptureBuffer &out)
{
    out.count = 0;
    HANDLE thread =
        OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                   static_cast<DWORD>(tid));
    if (thread == nullptr) {
        return false;
    }
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        CloseHandle(thread);
        return false;
    }

    CONTEXT context;
    ZeroMemory(&context, sizeof(context));
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context)) {
        STACKFRAME64 frame;
        ZeroMemory(&frame, sizeof(frame));
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;

        HANDLE process = GetCurrentProcess();
        std::size_t n = 0;
        while (n < CaptureBuffer::kMax) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
                break;
            }
            if (frame.AddrPC.Offset == 0) {
                break;
            }
            out.ips[n++] = static_cast<cpptrace::frame_ptr>(frame.AddrPC.Offset);
        }
        out.count = n;
    }

    ResumeThread(thread);
    CloseHandle(thread);
    return out.count > 0;
}

bool Capture::isThreadRunning(std::uint64_t /*tid*/)
{
    // TODO(windows): approximate via QueryThreadCycleTime deltas. For now, always sample.
    return true;
}

}  // namespace spark
