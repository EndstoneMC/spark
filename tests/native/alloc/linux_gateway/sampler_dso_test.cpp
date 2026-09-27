#include <dlfcn.h>
#include <pthread.h>

#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <thread>

#include <sys/stat.h>

#include "native/alloc/allocation_lifecycle_test_access.h"

namespace {
using Clock = std::chrono::steady_clock;
std::atomic<bool> ExitEntered{false};
std::atomic<bool> ExitRelease{false};
pthread_key_t ExitKey;
std::atomic<bool> WorkEntered{false};
std::atomic<bool> WorkRelease{false};

void holdWork() noexcept
{
    WorkEntered.store(true, std::memory_order_release);
    while (!WorkRelease.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

template <typename Predicate>
void waitFor(Predicate predicate)
{
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        require(Clock::now() < deadline, "fixture gate deadline");
        std::this_thread::yield();
    }
}

void exitDestructor(void *)
{
    ExitEntered.store(true);
    while (!ExitRelease.load()) {
        std::this_thread::yield();
    }
}

void armExit() noexcept
{
    ::pthread_setspecific(ExitKey, reinterpret_cast<void *>(1));
}

struct Fixture {
    void *handle;
    void (*create)(spark::test::LinuxAllocationTestControl *);
    int (*start)(unsigned);
    int (*finish)(bool);
    unsigned (*state)();
    unsigned (*group)();
    void (*tick)();
    void (*rescan)();
    std::uint64_t (*drops)();
    std::uint64_t (*age)();
    void (*destroy)();

    template <typename T>
    T symbol(const char *name)
    {
        const auto result = reinterpret_cast<T>(::dlsym(handle, name));
        require(result != nullptr, "sampler fixture API");
        return result;
    }
    explicit Fixture(spark::test::LinuxAllocationTestControl &control)
        : handle(::dlopen(SPARK_SAMPLER_FIXTURE, RTLD_NOW | RTLD_LOCAL))
    {
        if (handle == nullptr) {
            std::fprintf(stderr, "%s\n", ::dlerror());
        }
        require(handle != nullptr, "load actual sampler DSO");
        create = symbol<decltype(create)>("sampler_create");
        start = symbol<decltype(start)>("sampler_start");
        finish = symbol<decltype(finish)>("sampler_finish");
        state = symbol<decltype(state)>("sampler_state");
        group = symbol<decltype(group)>("sampler_group");
        tick = symbol<decltype(tick)>("sampler_tick");
        rescan = symbol<decltype(rescan)>("sampler_rescan");
        drops = symbol<decltype(drops)>("sampler_drops");
        age = symbol<decltype(age)>("sampler_age");
        destroy = symbol<decltype(destroy)>("sampler_destroy");
        create(&control);
    }
    ~Fixture()
    {
        require(finish(true) != 0, "actual sampler final shutdown");
        destroy();
        require(::dlclose(handle) == 0, "release actual sampler DSO");
        void *remaining = ::dlopen(SPARK_SAMPLER_FIXTURE, RTLD_NOW | RTLD_NOLOAD);
        require(remaining == nullptr, "actual sampler image absent after external release");
    }
};

void allocation()
{
    void *(*volatile allocate)(std::size_t) = &std::malloc;
    void (*volatile release)(void *) = &std::free;
    void *(*volatile zero)(std::size_t, std::size_t) = &std::calloc;
    void *(*volatile resize)(void *, std::size_t) = &std::realloc;
    void *pointer = allocate(4096);
    require(pointer != nullptr, "allocation result");
    release(pointer);
    pointer = zero(4, 1024);
    require(pointer != nullptr, "calloc result");
    pointer = resize(pointer, 8192);
    require(pointer != nullptr, "realloc result");
    release(pointer);
}

void verifyPreloadedAllocator()
{
    const char *expected = std::getenv("SPARK_EXPECT_ALLOCATOR");
    if (expected != nullptr) {
        struct stat expected_file{};
        require(::stat(expected, &expected_file) == 0, "expected allocator file");
        for (const char *name : {"malloc", "calloc", "realloc", "free"}) {
            Dl_info info{};
            require(::dladdr(::dlsym(RTLD_DEFAULT, name), &info) != 0, "resolved allocator owner");
            struct stat actual_file{};
            require(::stat(info.dli_fname, &actual_file) == 0 && actual_file.st_dev == expected_file.st_dev &&
                        actual_file.st_ino == expected_file.st_ino,
                    "actual allocator is preloaded provider");
        }
    }
}

void preloadAllocator()
{
    verifyPreloadedAllocator();
    auto *original_calloc = reinterpret_cast<void *(*)(std::size_t, std::size_t)>(::dlsym(RTLD_DEFAULT, "calloc"));
    errno = EDOM;
    require(original_calloc(std::numeric_limits<std::size_t>::max(), 2) == nullptr, "provider rejects calloc overflow");
    const int overflow_errno = errno;
    spark::test::LinuxAllocationTestControl control;
    Fixture fixture(control);
    const auto samples = fixture.symbol<std::uint64_t (*)()>("sampler_samples");
    const auto live_samples = fixture.symbol<std::uint64_t (*)()>("sampler_live_samples");
    const auto observed = fixture.symbol<std::uint64_t (*)()>("sampler_observed_bytes");
    const auto points = fixture.symbol<std::uint64_t (*)()>("sampler_sampling_points");
    const auto calls = fixture.symbol<std::uint64_t (*)()>("sampler_hook_calls");
    void *(*volatile allocate)(std::size_t) = nullptr;
    void *(*volatile zero_allocate)(std::size_t, std::size_t) = nullptr;
    void *(*volatile resize)(void *, std::size_t) = nullptr;
    void *(*volatile resize_array)(void *, std::size_t, std::size_t) = nullptr;
    void *(*volatile aligned_allocate)(std::size_t, std::size_t) = nullptr;
    void (*volatile release)(void *) = nullptr;
    for (int cycle = 0; cycle < 2; ++cycle) {
        require(fixture.start(0) != 0, "start preloaded allocation sampler");
        allocate = &std::malloc;
        zero_allocate = &std::calloc;
        resize = &std::realloc;
        resize_array = &::reallocarray;
        aligned_allocate = &::aligned_alloc;
        release = &std::free;
        const auto before_calls = calls();
        for (int i = 0; i < 32; ++i) {
            void *pointer = allocate(65536);
            require(pointer != nullptr, "preload malloc");
            std::memset(pointer, 0x5a, 65536);
            pointer = resize(pointer, 131072);
            require(pointer != nullptr, "preload realloc");
            release(pointer);
        }
        void *zero = zero_allocate(64, 1024);
        require(zero != nullptr && static_cast<unsigned char *>(zero)[0] == 0, "preload calloc");
        release(zero);
        void *array = allocate(65536);
        require(array != nullptr, "reallocarray source");
        array = resize_array(array, 128, 1024);
        require(array != nullptr, "preload reallocarray");
        release(array);
        void *aligned_array = aligned_allocate(64, 65536);
        require(aligned_array != nullptr && reinterpret_cast<std::uintptr_t>(aligned_array) % 64 == 0,
                "preload aligned_alloc");
        release(aligned_array);
        fixture.tick();
        require(calls() > before_calls && observed() > 0 && points() > 0, "allocator hooks sampled real calls");
        require(fixture.finish(false) != 0 && samples() != 0, "export nonzero allocation samples");
    }
    require(fixture.start(2) != 0, "start preloaded live-only sampler");
    allocate = &std::malloc;
    resize = &std::realloc;
    resize_array = &::reallocarray;
    release = &std::free;
    const auto baseline = live_samples();
    void *retained = allocate(131072);
    require(retained != nullptr && live_samples() > baseline, "retained allocation sampled");
    void *released = allocate(131072);
    require(released != nullptr, "released allocation sampled");
    const auto before_free = live_samples();
    release(released);
    require(live_samples() < before_free, "freed allocation retired");
    void *zeroed = allocate(131072);
    require(zeroed != nullptr, "zero-size realloc source");
    const auto before_zero = live_samples();
    void *zero_result = resize(zeroed, 0);
    require(live_samples() < before_zero, "zero-size realloc retires prior allocation");
    if (zero_result != nullptr) {
        release(zero_result);
    }
    void *zero_array = allocate(131072);
    require(zero_array != nullptr, "zero-size reallocarray source");
    const auto before_zero_array = live_samples();
    void *zero_array_result = resize_array(zero_array, 0, 2);
    require(live_samples() < before_zero_array, "zero-size reallocarray retires prior allocation");
    if (zero_array_result != nullptr) {
        release(zero_array_result);
    }
    void *failed = resize(retained, std::numeric_limits<std::size_t>::max());
    require(failed == nullptr && live_samples() > baseline, "failed realloc preserves old allocation");
    static_cast<unsigned char *>(retained)[0] = 0x5a;
    void *aligned = nullptr;
    require(::posix_memalign(&aligned, 64, 65536) == 0 && aligned != nullptr, "preload aligned allocation");
    std::thread releaser([&] { release(aligned); });
    releaser.join();
    errno = EDOM;
    void *overflow = zero_allocate(std::numeric_limits<std::size_t>::max(), 2);
    require(overflow == nullptr && errno == overflow_errno, "calloc overflow preserves provider errno");
    fixture.tick();
    require(fixture.finish(false) != 0 && samples() != 0, "export nonzero retained samples");
    release(retained);
}

void unsupportedZeroRealloc()
{
    verifyPreloadedAllocator();
    spark::test::LinuxAllocationTestControl control;
    Fixture fixture(control);
    require(fixture.start(0) == 0, "reject unsupported jemalloc zero_realloc configuration");
}

void churn()
{
    spark::test::LinuxAllocationTestControl control;
    Fixture fixture(control);
    for (unsigned cycle = 0; cycle < 6; ++cycle) {
        require(fixture.start(static_cast<unsigned>(cycle % 3 == 1)) != 0, "start actual sampler");
        for (unsigned i = 0; i < 32; ++i) {
            std::thread thread(allocation);
            thread.join();
        }
        fixture.tick();
        std::barrier entered(5);
        std::barrier stopped(5);
        std::array<std::thread, 4> exiting;
        for (auto &thread : exiting) {
            thread = std::thread([&] {
                allocation();
                entered.arrive_and_wait();
                stopped.arrive_and_wait();
            });
        }
        entered.arrive_and_wait();
        require(fixture.finish(false) != 0, "stop actual sampler");
        stopped.arrive_and_wait();
        for (auto &thread : exiting) {
            thread.join();
        }
        require(fixture.drops() == 0, "thread registry reclaimed during churn");
    }
    require(fixture.finish(true) != 0, "shutdown before restart");
    require(fixture.start(static_cast<unsigned>(false)) != 0 && fixture.finish(false) != 0,
            "restart after final shutdown");
}

void aggregatorExit()
{
    require(::pthread_key_create(&ExitKey, exitDestructor) == 0, "create thread exit fixture key");
    spark::test::LinuxAllocationTestControl control;
    control.aggregator_entry = armExit;
    {
        Fixture fixture(control);
        require(fixture.start(static_cast<unsigned>(false)) != 0, "start thread exit fixture");
        const auto before = Clock::now();
        require(fixture.finish(false) == 0, "held thread exit rejects stop");
        require(Clock::now() - before < std::chrono::seconds(6), "one bounded stop deadline");
        require(ExitEntered.load(), "actual pthread exit destructor reached");
        require((fixture.state() & 15) == 14, "thread exit ownership and timeout obligations retained");
        require(fixture.start(static_cast<unsigned>(false)) == 0, "restart blocked while exit is pending");
        ExitRelease.store(true);
        require(fixture.finish(false) != 0, "reap released aggregator on retry");
        require((fixture.state() & 15) == 8, "timeout latch survives completed cleanup");
    }
    require(::pthread_key_delete(ExitKey) == 0, "delete thread exit fixture key");
}

void startFailures()
{
    spark::test::LinuxAllocationTestControl control;
    Fixture fixture(control);
    for (unsigned failure = 1; failure <= 5; ++failure) {
        control.start_failure.store(failure);
        require(fixture.start(static_cast<unsigned>(false)) == 0, "injected actual start failure");
        require((fixture.state() & 23) == 0, "failed start fully retires TLS and producer resources");
        control.start_failure.store(0);
        require(fixture.start(static_cast<unsigned>(false)) != 0, "fresh session after failed start cleanup");
        require(fixture.finish(false) != 0, "successful session stop");
    }
    control.start_failure.store(2);
    control.key_delete_failure.store(true);
    require(fixture.start(static_cast<unsigned>(false)) == 0, "post-key start failure with failed key deletion");
    require((fixture.state() & 20) == 20, "key registry and cleanup obligation retained");
    const auto failed_group = fixture.group();
    require(fixture.start(static_cast<unsigned>(false)) == 0, "failed key deletion blocks restart");
    control.key_delete_failure.store(false);
    control.start_failure.store(0);
    require(fixture.finish(false) != 0, "retry completes failed-start final cleanup");
    require(fixture.start(static_cast<unsigned>(false)) != 0, "start after failed-start cleanup retry");
    require(fixture.group() != failed_group, "failed published group never reused");
    require(fixture.finish(false) != 0, "stop replacement group");
}

void loaderBlock()
{
    spark::test::LinuxAllocationTestControl control;
    Fixture fixture(control);
    require(fixture.start(0) != 0, "start loader blocking fixture");
    void *blocker = nullptr;
    std::thread loader([&] { blocker = ::dlopen(SPARK_LOADER_BLOCKER, RTLD_NOW | RTLD_LOCAL); });
    waitFor([] { return WorkEntered.load(); });
    fixture.rescan();
    waitFor([&] { return (fixture.state() & 32) != 0; });
    const auto ticks_started = Clock::now();
    for (unsigned i = 0; i < 100; ++i) {
        fixture.tick();
    }
    require(Clock::now() - ticks_started < std::chrono::milliseconds(200),
            "ticks stay prompt under actual loader lock");
    const auto stop_started = Clock::now();
    require(fixture.finish(false) == 0, "blocked production rescan retains stop obligation");
    require(Clock::now() - stop_started < std::chrono::seconds(6), "blocked loader stop deadline");
    const auto shutdown_started = Clock::now();
    require(fixture.finish(true) == 0, "blocked production rescan retains final shutdown obligation");
    require(Clock::now() - shutdown_started < std::chrono::seconds(6), "blocked loader shutdown deadline");
    require(fixture.start(0) == 0, "blocked loader prevents restart");
    WorkRelease.store(true);
    loader.join();
    require(blocker != nullptr, "blocking constructor completed");
    require(fixture.finish(true) != 0, "released loader permits final shutdown retry");
    require(::dlclose(blocker) == 0, "release blocking loader fixture");
}

void aggregationBlock(bool final)
{
    spark::test::LinuxAllocationTestControl control;
    if (final) {
        control.before_final_record = holdWork;
    }
    else {
        control.before_event = holdWork;
    }
    Fixture fixture(control);
    require(fixture.start(final ? 2 : 0) != 0, "start aggregation blocking fixture");
    void *retained = nullptr;
    std::thread producer([&] {
        if (final) {
            void *(*volatile allocate)(std::size_t) = &std::malloc;
            retained = allocate(32768);
        }
        else {
            allocation();
        }
    });
    producer.join();
    if (!final) {
        waitFor([] { return WorkEntered.load(); });
    }
    const auto before = Clock::now();
    require(fixture.finish(false) == 0, "held aggregation rejects stop");
    require(Clock::now() - before < std::chrono::seconds(6), "aggregation stop shares one deadline");
    require(WorkEntered.load(), "actual aggregation gate reached");
    require((fixture.state() & 15) == 14, "aggregation thread and pending session retained");
    require(fixture.start(0) == 0, "aggregation timeout blocks restart");
    WorkRelease.store(true);
    require(fixture.finish(false) != 0, "aggregation release permits retry");
    if (final) {
        require(fixture.age() < 2000, "delayed finalization uses frozen stop timestamp");
        std::free(retained);
    }
}

void handleFault()
{
    for (const char *point : {"50", "51"}) {
        spark::test::LinuxAllocationTestControl control;
        Fixture fixture(control);
        require(::setenv("SPARK_GATEWAY_HANDLE_FAULT", point, 1) == 0, "arm actual ELF pin acquisition failure");
        require(fixture.start(0) == 0, "actual sampler fails after ELF pin acquisition");
        ::unsetenv("SPARK_GATEWAY_HANDLE_FAULT");
        require((fixture.state() & 23) == 0, "failed ELF pin acquisition rolls back sampler TLS and storage");
    }
}

void callbackBlock(bool tls)
{
    spark::test::LinuxAllocationTestControl control;
    Fixture fixture(control);
    require(fixture.start(tls ? 0 : 1) != 0, "start actual callback blocking fixture");
    std::atomic<bool> ready{false};
    std::atomic<bool> run{false};
    std::thread producer([&] {
        if (tls) {
            allocation();
        }
        ready.store(true);
        while (!run.load()) {
            std::this_thread::yield();
        }
        if (!tls) {
            allocation();
        }
    });
    waitFor([&] { return ready.load(); });
    if (tls) {
        control.before_tls.store(holdWork);
    }
    else {
        control.before_hook.store(holdWork);
    }
    run.store(true);
    waitFor([] { return WorkEntered.load(); });
    fixture.rescan();
    const auto before = Clock::now();
    require(fixture.finish(tls) == 0, "held actual gateway callback rejects cleanup");
    require(Clock::now() - before < std::chrono::seconds(6), "held callback respects operation deadline");
    require((fixture.state() & 12) == 12 && fixture.start(0) == 0, "held callback retains epoch and blocks restart");
    WorkRelease.store(true);
    producer.join();
    require(fixture.finish(tls) != 0, "released actual callback permits cleanup retry");
}
void creationHeld()
{
    spark::test::LinuxAllocationTestControl control;
    Fixture fixture(control);
    const auto arm = fixture.symbol<void (*)(spark::test::StartFailureGate *)>("sampler_arm_creation");
    const auto hold = fixture.symbol<bool (*)(spark::test::TrackingGate *)>("sampler_hold_tracking");
    spark::test::StartFailureGate failure;
    spark::test::TrackingGate tracking;
    arm(&failure);
    bool started = true;
    const auto before = Clock::now();
    std::thread starter([&] { started = fixture.start(0) != 0; });
    waitFor([&] { return failure.before_thread_creation.load(); });
    std::thread holder([&] { require(hold(&tracking), "inject a prepublication producer obligation"); });
    waitFor([&] { return tracking.entered.load(); });
    failure.fail_now.store(true);
    starter.join();
    require(!started && Clock::now() - before < std::chrono::seconds(6), "worker creation failure is bounded");
    require((fixture.state() & 7) == 4 && fixture.start(0) == 0,
            "creation failure retains the producer epoch without an aggregator");
    tracking.release.store(true);
    holder.join();
    arm(nullptr);
    require(fixture.finish(false) != 0, "failed creation cleanup resumes after producer release");
    require(fixture.start(0) != 0 && fixture.finish(false) != 0, "fresh session follows creation rollback");
}

void snapshotPause(std::string_view mode)
{
    const bool shutdown = mode.ends_with("shutdown");
    const bool admitted = mode.find("admitted") != std::string_view::npos;
    spark::test::LinuxAllocationTestControl control;
    if (admitted) {
        control.snapshot_admitted = holdWork;
    }
    else if (mode.find("aggregate") != std::string_view::npos) {
        control.snapshot_before_aggregate = holdWork;
    }
    else {
        control.snapshot_before_restore = holdWork;
    }
    Fixture fixture(control);
    const auto snapshot = fixture.symbol<bool (*)()>("sampler_snapshot");
    const auto storage = fixture.symbol<bool (*)()>("sampler_storage");
    const auto suppress = fixture.symbol<bool (*)(bool)>("sampler_suppress");
    const auto samples = fixture.symbol<std::uint64_t (*)()>("sampler_samples");
    const auto current_thread_sampled = fixture.symbol<bool (*)()>("sampler_current_thread_sampled");
    require(fixture.start(2) != 0, "start retained snapshot fixture");
    const bool previous_suppression = suppress(true);
    require(!previous_suppression, "begin an external suppression pair before cleanup");
    void *retained = nullptr;
    std::thread producer([&] {
        void *(*volatile allocate)(std::size_t) = &std::malloc;
        retained = allocate(32768);
    });
    producer.join();
    require(retained != nullptr, "retain a real sampled allocation");
    bool captured = false;
    std::thread consumer([&] { captured = snapshot(); });
    waitFor([] { return WorkEntered.load(); });
    const auto before = Clock::now();
    require(fixture.finish(shutdown) == 0, "held snapshot rejects cleanup");
    require(Clock::now() - before < std::chrono::seconds(6), "snapshot cleanup shares one deadline");
    require((fixture.state() & 29) == 28 && storage(), "snapshot retains key, storage and cleanup obligation");
    require(!control.snapshot_restored.load(), "suppression guard remains alive inside consumer admission");
    require(!snapshot(), "closed consumer admission rejects another snapshot before TLS access");
    require(fixture.start(0) == 0, "snapshot blocks restart until consumer release");
    if (!shutdown) {
        require(suppress(previous_suppression), "restore suppression while ordinary cleanup is still pending");
        require(!suppress(false), "existing thread suppression flag is restored before consumer release");
    }
    else {
        require(!suppress(previous_suppression), "final shutdown rejects suppression access while key is retained");
    }
    std::thread unregistered(
        [&] { require(!suppress(true) && !suppress(false), "pending cleanup does not register a new TLS consumer"); });
    unregistered.join();
    WorkRelease.store(true);
    consumer.join();
    require(captured != admitted, "snapshot preserves state-check rejection and admitted completion semantics");
    require(control.snapshot_restored.load(), "suppression guard restores before consumer exits");
    require(fixture.finish(shutdown) != 0, "snapshot release permits cleanup retry");
    require(!storage() && samples() != 0, "cleanup preserves finalized profile exports");
    if (!shutdown) {
        require(!suppress(true) && suppress(false), "stopped TLS setters preserve paired suppression semantics");
    }
    else {
        require(!suppress(previous_suppression), "external suppression restore tolerates final key deletion");
        require(!suppress(true) && !suppress(false), "finalized TLS setters reject a deleted registry");
    }
    std::free(retained);
    require(fixture.start(0) != 0, "new session reopens consumer admission");
    allocation();
    require(fixture.finish(false) != 0, "stop the restarted session after actual thread allocations");
    require(current_thread_sampled(), "restored thread allocations appear in the restarted profile");
}

void requestStopLoader()
{
    spark::test::LinuxAllocationTestControl control;
    Fixture fixture(control);
    const auto request = fixture.symbol<void (*)()>("sampler_request_stop");
    request();
    require((fixture.state() & 5) == 0, "idle requestStop creates no cleanup obligation");
    require(fixture.start(1) != 0, "start count-only requestStop fixture");
    void *blocker = nullptr;
    std::thread loader([&] { blocker = ::dlopen(SPARK_LOADER_BLOCKER, RTLD_NOW | RTLD_LOCAL); });
    waitFor([] { return WorkEntered.load(); });
    fixture.rescan();
    waitFor([&] { return (fixture.state() & 32) != 0; });
    request();
    require((fixture.state() & 7) == 4, "requestStop publishes count-only cleanup before blocked rescan can finish");
    request();
    require((fixture.state() & 7) == 4, "repeated requestStop preserves pending epoch");
    WorkRelease.store(true);
    loader.join();
    require(blocker != nullptr && fixture.finish(false) != 0, "released rescan completes requested cleanup");
    require((fixture.state() & 5) == 0, "cleanup acknowledgement clears count-only pending state");
    request();
    require((fixture.state() & 5) == 0, "stopped requestStop remains idle");
    require(::dlclose(blocker) == 0, "release loader fixture after rescan cleanup");
}

void allocatorErrno()
{
    spark::test::LinuxAllocationTestControl control;
    Fixture fixture(control);
    const auto suppress = fixture.symbol<bool (*)(bool)>("sampler_suppress");
    const auto samples = fixture.symbol<std::uint64_t (*)()>("sampler_samples");
    const auto exercise = [] {
        std::array<int, 7> observed{};
        void *result = nullptr;
        errno = EDOM;
        result = std::malloc(8192);
        observed[0] = errno;
        require(result != nullptr, "errno malloc fixture");
        std::free(result);
        errno = EDOM;
        result = std::calloc(3, 8192);
        observed[1] = errno;
        require(result != nullptr, "errno calloc fixture");
        std::free(result);
        errno = EDOM;
        result = std::realloc(nullptr, 8192);
        observed[2] = errno;
        require(result != nullptr, "errno realloc fixture");
        std::free(result);
        void (*volatile free_call)(void *) = &std::free;
        errno = EDOM;
        free_call(nullptr);
        observed[3] = errno;
        errno = EDOM;
        result = ::reallocarray(nullptr, 3, 8192);
        observed[4] = errno;
        require(result != nullptr, "errno reallocarray fixture");
        std::free(result);
        errno = EDOM;
        result = std::aligned_alloc(16, 8192);
        observed[5] = errno;
        require(result != nullptr, "errno aligned allocation fixture");
        std::free(result);
        errno = EDOM;
        const auto status = ::posix_memalign(&result, 16, 8192);
        observed[6] = errno;
        require(status == 0 && result != nullptr, "errno posix_memalign fixture");
        std::free(result);
        return observed;
    };
    const auto expected = exercise();
    require(fixture.start(0) != 0, "start actual allocator errno fixture");
    require(exercise() == expected, "all allocator callbacks preserve original errno after bookkeeping");
    suppress(true);
    require(exercise() == expected, "suppressed allocator callbacks preserve original errno");
    suppress(false);
    require(fixture.finish(false) != 0 && samples() != 0, "errno exercise passed through actual sampled hooks");
    require(exercise() == expected, "closed allocator gateways preserve original errno");
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void spark_fixture_constructor_gate()
{
    holdWork();
}

int main(int argc, char **argv)
{
    require(argc == 2, "select fixture mode");
    const std::string_view mode(argv[1]);
    if (mode == "churn") {
        churn();
    }
    else if (mode == "aggregator_exit") {
        aggregatorExit();
    }
    else if (mode == "start_failures") {
        startFailures();
    }
    else if (mode == "loader_block") {
        loaderBlock();
    }
    else if (mode == "event_block" || mode == "final_block") {
        aggregationBlock(mode == "final_block");
    }
    else if (mode == "handle_fault") {
        handleFault();
    }
    else if (mode == "hook_block" || mode == "tls_block") {
        callbackBlock(mode == "tls_block");
    }
    else if (mode == "creation_held") {
        creationHeld();
    }
    else if (mode.starts_with("snapshot_")) {
        snapshotPause(mode);
    }
    else if (mode == "request_stop_loader") {
        requestStopLoader();
    }
    else if (mode == "errno") {
        allocatorErrno();
    }
    else if (mode == "preload") {
        preloadAllocator();
    }
    else if (mode == "preload_zero_realloc") {
        unsupportedZeroRealloc();
    }
    else {
        require(false, "known fixture mode");
    }
    std::puts("PASS: actual unloadable Linux sampler lifecycle");
}
