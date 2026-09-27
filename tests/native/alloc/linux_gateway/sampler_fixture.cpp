#include <algorithm>
#include <cstdio>
#include <string>

#include "native/alloc/allocation_diagnostics_test_access.h"
#include "native/alloc/allocation_lifecycle_test_access.h"
#include "native/sampler/thread_info.h"

namespace {
spark::AllocationSampler *Sampler = nullptr;
}

#define FIXTURE_API extern "C" __attribute__((visibility("default")))

FIXTURE_API void sampler_create(spark::test::LinuxAllocationTestControl *control)
{
    Sampler = new spark::AllocationSampler;
    spark::test::AllocationLifecycleTestAccess::configureLinux(*Sampler, control);
}

FIXTURE_API int sampler_start(unsigned mode)
{
    spark::AllocationSamplerConfig config;
    config.count_only = mode == 1;
    config.live_only = mode == 2;
    config.all_threads = true;
    config.interval_bytes = 1024;
    config.session_seed = 42;
    config.thread_state_limit_for_testing = 16;
    std::string error;
    const bool result = Sampler->start(config, error);
    if (!result) {
        std::fprintf(stderr, "sampler start: %s\n", error.c_str());
    }
    return static_cast<int>(result);
}

FIXTURE_API const char *sampler_backend_name()
{
    return Sampler->resolvedBackendName();
}

FIXTURE_API int sampler_finish(bool shutdown)
{
    std::string error;
    const bool result = shutdown ? Sampler->shutdown(error) : Sampler->stop(error);
    if (!result) {
        std::fprintf(stderr, "sampler finish: %s\n", error.c_str());
    }
    return static_cast<int>(result);
}

FIXTURE_API unsigned sampler_state()
{
    return (Sampler->running() ? 1 : 0) | (Sampler->aggregatorMayBeAlive() ? 2 : 0) |
           (Sampler->backendCleanupPending() ? 4 : 0) | (Sampler->stopWaitTimedOut() ? 8 : 0) |
           (spark::test::AllocationLifecycleTestAccess::linuxKeyCreated(*Sampler) ? 16 : 0) |
           (spark::test::AllocationLifecycleTestAccess::linuxRescanActive(*Sampler) ? 32 : 0);
}

FIXTURE_API unsigned sampler_group()
{
    return spark::test::AllocationLifecycleTestAccess::linuxGroup(*Sampler);
}

FIXTURE_API void sampler_tick()
{
    Sampler->onTick(50);
}
FIXTURE_API void sampler_rescan()
{
    spark::test::AllocationLifecycleTestAccess::requestLinuxRescan(*Sampler);
}
FIXTURE_API void sampler_request_stop()
{
    Sampler->requestStop();
}
FIXTURE_API bool sampler_snapshot()
{
    spark::AllocationSnapshot snapshot;
    std::string error;
    return Sampler->snapshot(snapshot, error);
}
FIXTURE_API bool sampler_storage()
{
    return spark::test::AllocationDiagnosticsTestAccess::fixtureStorageReady(*Sampler);
}
FIXTURE_API bool sampler_suppress(bool value)
{
    return Sampler->setCurrentThreadTrackingSuppressed(value);
}
FIXTURE_API std::uint64_t sampler_samples()
{
    return Sampler->sampleCount();
}
FIXTURE_API std::uint64_t sampler_live_samples()
{
    return Sampler->liveSamples();
}
FIXTURE_API std::uint64_t sampler_observed_bytes()
{
    return Sampler->observedBytes();
}
FIXTURE_API std::uint64_t sampler_sampling_points()
{
    return Sampler->samplingPoints();
}
FIXTURE_API std::uint64_t sampler_hook_calls()
{
    return Sampler->hookCalls();
}
FIXTURE_API bool sampler_current_thread_sampled()
{
    const auto marker = "(#" + std::to_string(spark::currentNativeThreadId()) + ", session #";
    return std::ranges::any_of(Sampler->threadTrees(), [&](const auto &entry) {
        const auto &thread = entry.second;
        return thread.thread_name.find(marker) != std::string::npos && !thread.tree.empty();
    });
}
FIXTURE_API void sampler_arm_creation(spark::test::StartFailureGate *gate)
{
    if (gate != nullptr) {
        spark::test::AllocationLifecycleTestAccess::armThreadCreationFailure(*Sampler, *gate);
    }
    else {
        spark::test::AllocationLifecycleTestAccess::disarmThreadCreationFailure(*Sampler);
    }
}
FIXTURE_API bool sampler_hold_tracking(spark::test::TrackingGate *gate)
{
    return spark::test::AllocationLifecycleTestAccess::holdTrackingCall(*Sampler, *gate);
}
FIXTURE_API std::uint64_t sampler_drops()
{
    return Sampler->threadStateDrops();
}
FIXTURE_API std::uint64_t sampler_age()
{
    return Sampler->retainedMaximumAgeMs();
}
FIXTURE_API void sampler_destroy()
{
    delete Sampler;
    Sampler = nullptr;
}
