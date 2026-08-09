#include <fcntl.h>
#include <semaphore.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include <cpptrace/cpptrace.hpp>
#include <sys/syscall.h>

#include "native/sampler/capture.h"

namespace spark {

namespace {

constexpr int KSignal = SIGPROF;

constexpr std::uint64_t KPhaseMask = 3;
constexpr std::uint64_t KRequested = 1;
constexpr std::uint64_t KCapturing = 2;
constexpr std::uint64_t KComplete = 3;

std::atomic<std::uint64_t> GState{0};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
std::atomic<std::uint32_t> GNextToken{1};
std::atomic<std::uint32_t> GActiveCaptureCalls{0};
CaptureBuffer GResult;
sem_t GDone;
std::atomic<bool> GArmed{false};
struct sigaction GPreviousAction{};
std::atomic<std::atomic<bool> *> GTestHandlerEntered{nullptr};
std::atomic<std::atomic<bool> *> GTestHandlerRelease{nullptr};
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<std::atomic<bool> *>::is_always_lock_free);

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
    std::uint64_t expected = captureState(token, KRequested);
    if (!GState.compare_exchange_strong(expected, captureState(token, KCapturing), std::memory_order_acq_rel)) {
        return;
    }
    if (auto *entered = GTestHandlerEntered.load(std::memory_order_acquire)) {
        entered->store(true, std::memory_order_release);
        auto *release = GTestHandlerRelease.load(std::memory_order_acquire);
        while (release != nullptr && !release->load(std::memory_order_acquire)) {
        }
    }
    GResult.count = cpptrace::safe_generate_raw_trace(GResult.ips, CaptureBuffer::kMax, 0);
    GState.store(captureState(token, KComplete), std::memory_order_release);
    sem_post(&GDone);
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
    while (GState.load(std::memory_order_acquire) != complete_state) {
        if (sem_timedwait(&GDone, &deadline) == 0) {
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
    if (GArmed.load()) {
        return true;
    }
    if (!cpptrace::can_signal_safe_unwind()) {
        return false;
    }
    if (sem_init(&GDone, 0, 0) != 0) {
        return false;
    }

    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(KSignal, &sa, &GPreviousAction) != 0) {
        sem_destroy(&GDone);
        return false;
    }

    // Warm the safe path before the first signal-handler capture.
    cpptrace::frame_ptr warm[8];
    std::size_t n = cpptrace::safe_generate_raw_trace(warm, 8);
    if (n > 0) {
        cpptrace::safe_object_frame frame;
        cpptrace::get_safe_object_frame(warm[0], &frame);
    }

    GArmed.store(true);
    return true;
}

bool Capture::disarm()
{
    if (!GArmed.exchange(false)) {
        return true;
    }
    if (GActiveCaptureCalls.load(std::memory_order_acquire) != 0) {
        GArmed.store(true, std::memory_order_release);
        return false;
    }
    std::uint64_t state = GState.load(std::memory_order_acquire);
    if ((state & KPhaseMask) == KRequested) {
        GState.compare_exchange_strong(state, 0, std::memory_order_acq_rel);
    }
    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    sigaction(KSignal, &ignored, nullptr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((GState.load(std::memory_order_acquire) & KPhaseMask) == KCapturing &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if ((GState.load(std::memory_order_acquire) & KPhaseMask) == KCapturing) {
        GArmed.store(true, std::memory_order_release);
        return false;
    }
    sigaction(KSignal, &GPreviousAction, nullptr);
    GState.store(0, std::memory_order_release);
    sem_destroy(&GDone);
    return true;
}

void Capture::setHandlerGateForTesting(std::atomic<bool> *entered, std::atomic<bool> *release)
{
    GTestHandlerRelease.store(release, std::memory_order_release);
    GTestHandlerEntered.store(entered, std::memory_order_release);
}

bool Capture::captureThread(std::uint64_t tid, CaptureBuffer &out)
{
    GActiveCaptureCalls.fetch_add(1, std::memory_order_acq_rel);
    struct ActiveCallGuard {
        ~ActiveCallGuard() { GActiveCaptureCalls.fetch_sub(1, std::memory_order_release); }
    } active_call_guard;
    if (!GArmed.load(std::memory_order_acquire)) {
        return false;
    }
    if ((GState.load(std::memory_order_acquire) & KPhaseMask) == KCapturing) {
        return false;
    }
    while (sem_trywait(&GDone) == 0) {
    }
    std::uint32_t token = GNextToken.fetch_add(1, std::memory_order_relaxed);
    if (token == 0) {
        token = GNextToken.fetch_add(1, std::memory_order_relaxed);
    }
    const std::uint64_t requested_state = captureState(token, KRequested);
    const std::uint64_t capturing_state = captureState(token, KCapturing);
    const std::uint64_t complete_state = captureState(token, KComplete);
    GResult.count = 0;
    GState.store(requested_state, std::memory_order_release);

    siginfo_t info{};
    info.si_signo = KSignal;
    info.si_code = SI_QUEUE;
    info.si_pid = getpid();
    info.si_uid = getuid();
    info.si_value.sival_int = static_cast<int>(token);
    if (syscall(SYS_rt_tgsigqueueinfo, getpid(), static_cast<pid_t>(tid), KSignal, &info) != 0) {
        GState.store(0, std::memory_order_release);
        return false;
    }

    const timespec deadline = deadlineAfterOneSecond();
    if (!waitForCompletion(deadline, complete_state)) {
        std::uint64_t expected = requested_state;
        if (GState.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
            return false;
        }
        if (expected == capturing_state) {
            if (!waitForCompletion(deadlineAfterOneSecond(), complete_state)) {
                return false;
            }
        }
        GState.store(0, std::memory_order_release);
        return false;
    }

    out = GResult;
    GState.store(0, std::memory_order_release);
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
