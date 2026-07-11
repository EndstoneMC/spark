#include "sampler/capture.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cpptrace/cpptrace.hpp>

namespace spark {

namespace {

constexpr int kSignal = SIGPROF;

// Only one capture is ever in flight (the sampler thread serializes), so a single
// target slot + semaphore is a sufficient, async-signal-safe handshake.
std::atomic<CaptureBuffer *> g_target{nullptr};
sem_t g_done;
std::atomic<bool> g_armed{false};

void handler(int, siginfo_t *, void *)
{
    CaptureBuffer *buf = g_target.load(std::memory_order_acquire);
    if (buf != nullptr) {
        // The only async-signal-safe work: walk the stack into a fixed buffer.
        buf->count = cpptrace::safe_generate_raw_trace(buf->ips, CaptureBuffer::kMax, 0);
    }
    sem_post(&g_done);  // async-signal-safe
}

}  // namespace

bool Capture::arm()
{
    if (g_armed.load()) {
        return true;
    }
    if (!cpptrace::can_signal_safe_unwind()) {
        return false;
    }
    if (sem_init(&g_done, 0, 0) != 0) {
        return false;
    }

    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(kSignal, &sa, nullptr) != 0) {
        return false;
    }

    // Warm up the safe path so the first real sample doesn't fault in lazy loader
    // state inside the handler.
    cpptrace::frame_ptr warm[8];
    std::size_t n = cpptrace::safe_generate_raw_trace(warm, 8);
    if (n > 0) {
        cpptrace::safe_object_frame frame;
        cpptrace::get_safe_object_frame(warm[0], &frame);
    }

    g_armed.store(true);
    return true;
}

void Capture::disarm()
{
    if (!g_armed.load()) {
        return;
    }
    signal(kSignal, SIG_DFL);
    g_armed.store(false);
}

bool Capture::captureThread(std::uint64_t tid, CaptureBuffer &out)
{
    if (!g_armed.load()) {
        return false;
    }
    out.count = 0;
    g_target.store(&out, std::memory_order_release);

    if (syscall(SYS_tgkill, getpid(), static_cast<pid_t>(tid), kSignal) != 0) {
        g_target.store(nullptr, std::memory_order_release);
        return false;
    }

    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;  // generous timeout; a stuck main thread must not hang the sampler
    while (sem_timedwait(&g_done, &ts) != 0) {
        if (errno == EINTR) {
            continue;
        }
        g_target.store(nullptr, std::memory_order_release);
        return false;
    }

    g_target.store(nullptr, std::memory_order_release);
    return out.count > 0;
}

bool Capture::isThreadRunning(std::uint64_t tid)
{
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%llu/stat", static_cast<unsigned long long>(tid));
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return true;
    }
    char buf[256];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) {
        return true;
    }
    buf[n] = '\0';
    // stat layout: "pid (comm) STATE ..." — comm may contain spaces/parens, so scan
    // from the last ')'.
    char *p = std::strrchr(buf, ')');
    if (p == nullptr || p[1] == '\0' || p[2] == '\0') {
        return true;
    }
    char state = p[2];
    return state == 'R';
}

}  // namespace spark
