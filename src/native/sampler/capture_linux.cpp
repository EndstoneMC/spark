#include <fcntl.h>
#include <semaphore.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <cpptrace/cpptrace.hpp>
#include <sys/syscall.h>

#include "native/sampler/capture.h"

namespace spark {

namespace {

constexpr int kSignal = SIGPROF;

constexpr std::uint64_t kPhaseMask = 3;
constexpr std::uint64_t kRequested = 1;
constexpr std::uint64_t kCapturing = 2;
constexpr std::uint64_t kComplete = 3;

std::atomic<std::uint64_t> g_state{0};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
std::atomic<std::uint32_t> g_next_token{1};
CaptureBuffer g_result;
sem_t g_done;
std::atomic<bool> g_armed{false};
struct sigaction g_previous_action{};

std::uint64_t captureState(std::uint32_t token, std::uint64_t phase)
{
    return (static_cast<std::uint64_t>(token) << 2) | phase;
}

void handler(int, siginfo_t *info, void *)
{
    if (info == nullptr || info->si_code != SI_QUEUE) {
        return;
    }
    const auto token = static_cast<std::uint32_t>(info->si_value.sival_int);
    std::uint64_t expected = captureState(token, kRequested);
    if (!g_state.compare_exchange_strong(expected, captureState(token, kCapturing), std::memory_order_acq_rel)) {
        return;
    }
    g_result.count = cpptrace::safe_generate_raw_trace(g_result.ips, CaptureBuffer::kMax, 0);
    g_state.store(captureState(token, kComplete), std::memory_order_release);
    sem_post(&g_done);
}

timespec deadlineAfterOneSecond()
{
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    ++deadline.tv_sec;
    return deadline;
}

bool waitForCompletion(const timespec &deadline, std::uint64_t complete_state)
{
    while (g_state.load(std::memory_order_acquire) != complete_state) {
        if (sem_timedwait(&g_done, &deadline) == 0) {
            continue;
        }
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
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
    if (sigaction(kSignal, &sa, &g_previous_action) != 0) {
        sem_destroy(&g_done);
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
    if (!g_armed.exchange(false)) {
        return;
    }
    std::uint64_t state = g_state.load(std::memory_order_acquire);
    if ((state & kPhaseMask) == kRequested) {
        g_state.compare_exchange_strong(state, 0, std::memory_order_acq_rel);
    }
    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    sigaction(kSignal, &ignored, nullptr);
    while ((g_state.load(std::memory_order_acquire) & kPhaseMask) == kCapturing) {
        const timespec deadline = deadlineAfterOneSecond();
        if (sem_timedwait(&g_done, &deadline) != 0 && errno != EINTR) {
            break;
        }
    }
    sigaction(kSignal, &g_previous_action, nullptr);
    g_state.store(0, std::memory_order_release);
    sem_destroy(&g_done);
}

bool Capture::captureThread(std::uint64_t tid, CaptureBuffer &out)
{
    if (!g_armed.load()) {
        return false;
    }
    if ((g_state.load(std::memory_order_acquire) & kPhaseMask) == kCapturing) {
        return false;
    }
    while (sem_trywait(&g_done) == 0) {
    }
    std::uint32_t token = g_next_token.fetch_add(1, std::memory_order_relaxed);
    if (token == 0) {
        token = g_next_token.fetch_add(1, std::memory_order_relaxed);
    }
    const std::uint64_t requested_state = captureState(token, kRequested);
    const std::uint64_t capturing_state = captureState(token, kCapturing);
    const std::uint64_t complete_state = captureState(token, kComplete);
    g_result.count = 0;
    g_state.store(requested_state, std::memory_order_release);

    siginfo_t info{};
    info.si_signo = kSignal;
    info.si_code = SI_QUEUE;
    info.si_pid = getpid();
    info.si_uid = getuid();
    info.si_value.sival_int = static_cast<int>(token);
    if (syscall(SYS_rt_tgsigqueueinfo, getpid(), static_cast<pid_t>(tid), kSignal, &info) != 0) {
        g_state.store(0, std::memory_order_release);
        return false;
    }

    const timespec deadline = deadlineAfterOneSecond();
    if (!waitForCompletion(deadline, complete_state)) {
        std::uint64_t expected = requested_state;
        if (g_state.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
            return false;
        }
        if (expected == capturing_state) {
            if (!waitForCompletion(deadlineAfterOneSecond(), complete_state)) {
                return false;
            }
        }
        g_state.store(0, std::memory_order_release);
        return false;
    }

    out = g_result;
    g_state.store(0, std::memory_order_release);
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
