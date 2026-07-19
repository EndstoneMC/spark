#include "sampler/capture.h"

// Windows stack capture: there are no POSIX signals, so we cannot run code on the
// target thread at a sampling instant. Instead the sampler thread suspends the target,
// reads its CONTEXT, and walks the stack with StackWalk64, then resumes it. The raw
// addresses are converted to module-relative keys and resolved later through DbgHelp.

#include <mutex>
#include <unordered_map>

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

namespace spark {

namespace {
std::mutex g_dbghelp_mutex;
bool g_armed = false;
std::unordered_map<DWORD, ULONG64> g_thread_cycles;
}  // namespace

bool Capture::arm()
{
    std::lock_guard lock(g_dbghelp_mutex);
    if (g_armed) {
        return true;
    }
    SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {  // needed by StackWalk64's module callbacks
        return false;
    }
    g_thread_cycles.clear();
    g_armed = true;
    return true;
}

void Capture::disarm()
{
    std::lock_guard lock(g_dbghelp_mutex);
    if (!g_armed) {
        return;
    }
    SymCleanup(GetCurrentProcess());
    g_thread_cycles.clear();
    g_armed = false;
}

bool Capture::captureThread(std::uint64_t tid, CaptureBuffer &out)
{
    std::lock_guard lock(g_dbghelp_mutex);
    out.count = 0;
    if (!g_armed) {
        return false;
    }

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

    bool resumed = ResumeThread(thread) != static_cast<DWORD>(-1);
    CloseHandle(thread);
    return resumed && out.count > 0;
}

bool Capture::isThreadRunning(std::uint64_t tid)
{
    std::lock_guard lock(g_dbghelp_mutex);
    if (!g_armed) {
        return true;
    }

    const DWORD thread_id = static_cast<DWORD>(tid);
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

    auto it = g_thread_cycles.find(thread_id);
    if (it == g_thread_cycles.end()) {
        g_thread_cycles.emplace(thread_id, cycles);
        return false;  // the first observation establishes a baseline
    }
    const ULONG64 previous = it->second;
    it->second = cycles;
    return cycles != previous;
}

}  // namespace spark
