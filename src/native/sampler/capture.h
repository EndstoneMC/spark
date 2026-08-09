#ifndef ENDSTONE_SPARK_CAPTURE_H
#define ENDSTONE_SPARK_CAPTURE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <cpptrace/cpptrace.hpp>

namespace spark {

struct CaptureTestAccess;

// One captured stack, as raw instruction pointers (leaf..root). Resolving each ip
// to a module + RVA is done by the sampler thread, off the capture path.
struct CaptureBuffer {
    static constexpr std::size_t kMax = 256;
    cpptrace::frame_ptr ips[kMax];
    std::size_t count = 0;
};

// Platform stack-capture backend.
//   Linux:   SIGPROF delivered to the target thread; cpptrace safe unwind in-handler.
//   Windows: SuspendThread + StackWalk64 on the target's CONTEXT (see capture_windows.cpp).
class Capture {
public:
    // Install the handler / prime cpptrace's signal-safe path. Returns false if
    // async-signal-safe unwinding is unavailable (cpptrace not built with libunwind).
    static bool arm();
    static bool disarm();

    // Capture the stack of the given OS thread id into `out`. Called from the
    // sampler thread. Returns false on timeout/failure.
    static bool captureThread(std::uint64_t tid, CaptureBuffer &out);

    // True if the thread is currently on-CPU (for --ignore-sleeping gating).
    static bool isThreadRunning(std::uint64_t tid);

private:
    friend struct CaptureTestAccess;
#ifdef __linux__
    static void setHandlerGateForTesting(std::atomic<bool> *entered, std::atomic<bool> *release);
#endif
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_CAPTURE_H
