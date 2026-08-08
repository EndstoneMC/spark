#include "native/sampler/capture.h"

// Windows stack capture: suspend target thread, walk with StackWalk64, resume.

#include <mutex>
#include <unordered_map>

#include "native/symbol/dbghelp_manager.h"

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

namespace spark {

namespace {
bool GArmed = false;
std::unordered_map<DWORD, ULONG64> GThreadCycles;  // NOLINT(bugprone-throwing-static-initialization)
}  // namespace

bool Capture::arm()
{
    {
        std::scoped_lock lock(dbgHelpMutex());
        if (GArmed) {
            return true;
        }
    }
    if (!retainDbgHelp()) {
        return false;
    }
    std::scoped_lock lock(dbgHelpMutex());
    GThreadCycles.clear();
    GArmed = true;
    return true;
}

void Capture::disarm()
{
    {
        std::scoped_lock lock(dbgHelpMutex());
        if (!GArmed) {
            return;
        }
        GThreadCycles.clear();
        GArmed = false;
    }
    releaseDbgHelp();
}

bool Capture::captureThread(std::uint64_t tid, CaptureBuffer &out)
{
    std::scoped_lock lock(dbgHelpMutex());
    out.count = 0;
    if (!GArmed) {
        return false;
    }

    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
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
        // The suspended CONTEXT is already a valid sample. Keep its instruction
        // pointer even when StackWalk64 cannot unwind a caller (for example in
        // optimized system/runtime code without usable unwind metadata).
        if (context.Rip != 0) {
            out.ips[n++] = static_cast<cpptrace::frame_ptr>(context.Rip);
        }
        while (n < CaptureBuffer::kMax) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
                break;
            }
            if (frame.AddrPC.Offset == 0) {
                break;
            }
            const auto ip = static_cast<cpptrace::frame_ptr>(frame.AddrPC.Offset);
            if (n == 0 || ip != out.ips[n - 1]) {
                out.ips[n++] = ip;
            }
        }
        out.count = n;
    }

    bool resumed = ResumeThread(thread) != static_cast<DWORD>(-1);
    CloseHandle(thread);
    return resumed && out.count > 0;
}

bool Capture::isThreadRunning(std::uint64_t tid)
{
    std::scoped_lock lock(dbgHelpMutex());
    if (!GArmed) {
        return true;
    }

    const auto thread_id = static_cast<DWORD>(tid);
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
    if (thread == nullptr) {
        return true;
    }
    ULONG64 cycles = 0;
    const bool queried = QueryThreadCycleTime(thread, &cycles) != FALSE;
    CloseHandle(thread);
    if (!queried) {
        return true;
    }

    auto it = GThreadCycles.find(thread_id);
    if (it == GThreadCycles.end()) {
        GThreadCycles.emplace(thread_id, cycles);
        return false;  // the first observation establishes a baseline
    }
    const ULONG64 previous = it->second;
    it->second = cycles;
    return cycles != previous;
}

}  // namespace spark
