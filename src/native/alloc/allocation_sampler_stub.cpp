#include <memory>
#include <utility>

#include "native/alloc/allocation_sampler.h"

#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
#include "native/alloc/allocation_diagnostics_test_access.h"
#include "native/alloc/allocation_lifecycle_test_access.h"
#endif

namespace spark {

// NOLINTBEGIN(readability-convert-member-functions-to-static)

struct AllocationSampler::Impl {
    CallTree tree;
    std::map<std::uint64_t, ThreadCallTree> thread_trees;
    ModuleTable modules;
    std::map<std::int32_t, WindowTickStats> windows;
    std::vector<AllocationHookCapability> capabilities;
};

AllocationSampler::AllocationSampler() : impl_(std::make_unique<Impl>()) {}

AllocationSampler::~AllocationSampler() = default;

bool AllocationSampler::start(const AllocationSamplerConfig &, std::string &error)
{
#ifdef _WIN32
    error = "Windows allocation profiling is unavailable on this architecture";
#else
    error = "native allocation profiling is supported only on Windows x64 and Linux x86-64";
#endif
    return false;
}

bool AllocationSampler::stop(std::string &error)
{
    error.clear();
    return true;
}

void AllocationSampler::requestStop() noexcept {}

bool AllocationSampler::shutdown(std::string &error)
{
    error.clear();
    return true;
}

void AllocationSampler::onTick(double) {}

bool AllocationSampler::snapshot(AllocationSnapshot &, std::string &error)
{
    error = "native allocation profiling is not supported on this platform";
    return false;
}

AllocationDiagnostics AllocationSampler::diagnostics() const
{
    AllocationDiagnostics result;
    result.accounting_state = AllocationAccountingState::NotApplicable;
    return result;
}

AllocationDiagnosticsSnapshot AllocationSampler::allocationDiagnostics() const
{
    return diagnostics();
}

bool AllocationSampler::setCurrentThreadTrackingSuppressed(bool) noexcept
{
    return false;
}

void AllocationSampler::setRecoverySink(RecoverySink *) {}

const CallTree &AllocationSampler::tree() const
{
    return impl_->tree;
}

const std::map<std::uint64_t, ThreadCallTree> &AllocationSampler::threadTrees() const
{
    return impl_->thread_trees;
}

const ModuleTable &AllocationSampler::modules() const
{
    return impl_->modules;
}

const std::map<std::int32_t, WindowTickStats> &AllocationSampler::windowTicks() const
{
    return impl_->windows;
}

std::uint64_t AllocationSampler::numberOfTicks() const
{
    return 0;
}

std::uint64_t AllocationSampler::hookCalls() const
{
    return 0;
}
std::uint64_t AllocationSampler::successfulAllocationCalls() const
{
    return 0;
}

std::uint64_t AllocationSampler::sampleCount() const
{
    return 0;
}

std::uint64_t AllocationSampler::samplingPoints() const
{
    return 0;
}

std::uint64_t AllocationSampler::sampledBytes() const
{
    return 0;
}
std::uint64_t AllocationSampler::filteredSamples() const
{
    return 0;
}
std::uint64_t AllocationSampler::threadNameFailures() const
{
    return 0;
}
std::uint64_t AllocationSampler::threadIdentityCacheDrops() const
{
    return 0;
}

std::uint64_t AllocationSampler::observedBytes() const
{
    return 0;
}

std::uint64_t AllocationSampler::droppedSamples() const
{
    return 0;
}

std::uint64_t AllocationSampler::enqueuedSamples() const
{
    return 0;
}
std::uint64_t AllocationSampler::droppedEvents() const
{
    return 0;
}
std::uint64_t AllocationSampler::droppedTickEvents() const
{
    return 0;
}
std::uint64_t AllocationSampler::tickEventCapacity()
{
    return 0;
}
std::uint64_t AllocationSampler::eventQueueHighWaterMark() const
{
    return 0;
}
std::uint64_t AllocationSampler::eventQueueCapacity()
{
    return 0;
}
std::uint64_t AllocationSampler::freedSamples() const
{
    return 0;
}
std::uint64_t AllocationSampler::freedBytes() const
{
    return 0;
}
std::uint64_t AllocationSampler::liveSamples() const
{
    return 0;
}
std::uint64_t AllocationSampler::liveBytes() const
{
    return 0;
}
std::uint64_t AllocationSampler::peakLiveSamples() const
{
    return 0;
}
std::uint64_t AllocationSampler::liveIndexCapacity()
{
    return 0;
}
std::uint64_t AllocationSampler::liveRecordCapacity()
{
    return 0;
}
std::uint64_t AllocationSampler::moduleCacheCapacity()
{
    return 0;
}
std::uint64_t AllocationSampler::sampledThreadCount() const
{
    return 0;
}
std::uint64_t AllocationSampler::threadRootCapacity()
{
    return 0;
}
std::uint64_t AllocationSampler::overflowThreadCount() const
{
    return 0;
}
std::uint64_t AllocationSampler::threadStateDrops() const
{
    return 0;
}
std::uint64_t AllocationSampler::hookedModuleCount() const
{
    return 0;
}
std::uint64_t AllocationSampler::skippedModuleCount() const
{
    return 0;
}
std::uint64_t AllocationSampler::failedModuleCount() const
{
    return 0;
}
std::uint64_t AllocationSampler::moduleRegistryCount() const
{
    return 0;
}
std::uint64_t AllocationSampler::moduleRegistryCapacity()
{
    return 0;
}
std::uint64_t AllocationSampler::profileNodeCapacity()
{
    return 0;
}
std::uint64_t AllocationSampler::profileTimeEntryCapacity()
{
    return 0;
}
std::uint64_t AllocationSampler::profileStorageSampleDrops() const
{
    return 0;
}
bool AllocationSampler::profileStorageExhausted() const
{
    return false;
}
std::uint64_t AllocationSampler::pendingSampleCapacity()
{
    return 0;
}
std::uint64_t AllocationSampler::pendingSampleDrops() const
{
    return 0;
}
std::uint64_t AllocationSampler::pendingCapacityDrops() const
{
    return 0;
}
std::uint64_t AllocationSampler::pendingStaleDrops() const
{
    return 0;
}
std::uint64_t AllocationSampler::terminalInFlightTickSamplesDiscarded() const
{
    return 0;
}
std::uint64_t AllocationSampler::pendingFinalDrops() const
{
    return 0;
}
std::uint64_t AllocationSampler::moduleOverflowFrames() const
{
    return 0;
}
std::uint64_t AllocationSampler::retainedHistoryWindows() const
{
    return 0;
}
std::uint64_t AllocationSampler::historySamplesPruned() const
{
    return 0;
}
std::uint64_t AllocationSampler::historyBytesPruned() const
{
    return 0;
}
bool AllocationSampler::historyTruncated() const
{
    return false;
}
bool AllocationSampler::dataIncomplete() const
{
    return false;
}
std::uint64_t AllocationSampler::averageLifetimeMs() const
{
    return 0;
}
std::uint64_t AllocationSampler::maximumLifetimeMs() const
{
    return 0;
}
std::uint64_t AllocationSampler::lifecycleDropped() const
{
    return 0;
}
std::uint64_t AllocationSampler::contentionDropped() const
{
    return 0;
}

std::uint64_t AllocationSampler::drainTruncated() const
{
    return 0;
}

bool AllocationSampler::stopWaitTimedOut() const
{
    return false;
}

bool AllocationSampler::aggregatorMayBeAlive() const
{
    return false;
}

bool AllocationSampler::backendCleanupPending() const
{
    return false;
}

#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
namespace test {

bool AllocationDiagnosticsTestAccess::configureFixture(AllocationSampler &, bool, bool,
                                                       AllocationFixtureWorkerGate *) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::releaseFixture(AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::fixtureStorageReady(const AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::fixtureWorkerPresent(const AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::fixtureHooksPresent(const AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::fixtureAggregatorRunning(const AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::seedFixtureQueues(AllocationSampler &, AllocationFixtureSeedCounts &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::seedFixtureQueues(AllocationSampler &, AllocationFixtureSeedCounts &,
                                                        const AllocationFixtureSeedCounts &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::recordFixtureAllocation(AllocationSampler &, void *, std::uint64_t) noexcept
{
    return false;
}

bool AllocationLifecycleTestAccess::holdTrackingCall(AllocationSampler &, TrackingGate &gate) noexcept
{
    gate.exited.store(true, std::memory_order_release);
    return false;
}

void AllocationLifecycleTestAccess::armThreadCreationFailure(AllocationSampler &, StartFailureGate &) noexcept {}

void AllocationLifecycleTestAccess::disarmThreadCreationFailure(AllocationSampler &) noexcept {}

void AllocationDiagnosticsTestAccess::forceAggregatorCpuReadFailure(AllocationSampler &, bool) noexcept {}

void AllocationDiagnosticsTestAccess::forceAggregatorCpuZero(AllocationSampler &, bool) noexcept {}

void AllocationDiagnosticsTestAccess::armEventProcessingGate(AllocationSampler &,
                                                             AllocationEventProcessingGate &gate) noexcept
{
    gate.entered.store(false, std::memory_order_relaxed);
    gate.release.store(false, std::memory_order_relaxed);
    gate.timed_out.store(false, std::memory_order_relaxed);
}

void AllocationDiagnosticsTestAccess::disarmEventProcessingGate(AllocationSampler &) noexcept {}

void AllocationDiagnosticsTestAccess::forceProcessEventFailure(AllocationSampler &, bool) noexcept {}

bool AllocationDiagnosticsTestAccess::configureFixtureCpuWork(AllocationSampler &,
                                                              AllocationFixtureCpuWorkControl &) noexcept
{
    return false;
}

void AllocationDiagnosticsTestAccess::forceDrainDeadline(AllocationSampler &, bool) noexcept {}

void AllocationDiagnosticsTestAccess::forceRetainedWalkBudget(AllocationSampler &, bool) noexcept {}

std::uint64_t AllocationDiagnosticsTestAccess::retainedWalkVisits(const AllocationSampler &) noexcept
{
    return 0;
}

bool AllocationDiagnosticsTestAccess::drainFixtureAggregatorContext(AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::seedFixtureLiveAllocations(AllocationSampler &, void *const *,
                                                                 std::size_t) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::prepareFixtureLiveRecord(AllocationSampler &, void *, std::uint64_t,
                                                               AllocationFixtureLiveRecord &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::holdInsertionShardLock(AllocationSampler &, void *,
                                                             AllocationFixtureLockGate &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::holdDetachShardLock(AllocationSampler &, void *,
                                                          AllocationFixtureLockGate &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::insertFixtureLiveRecord(AllocationSampler &, AllocationFixtureLiveRecord &,
                                                              bool) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::detachFixtureLiveRecord(AllocationSampler &,
                                                              AllocationFixtureLiveRecord &) noexcept
{
    return false;
}

void AllocationDiagnosticsTestAccess::retireFixtureLiveRecord(AllocationSampler &,
                                                              AllocationFixtureLiveRecord &) noexcept
{
}

void AllocationDiagnosticsTestAccess::releaseFixtureLiveRecord(AllocationSampler &,
                                                               AllocationFixtureLiveRecord &) noexcept
{
}

bool AllocationDiagnosticsTestAccess::fileTimeToNanoseconds(std::uint32_t, std::uint32_t, std::uint64_t &value) noexcept
{
    value = 0;
    return false;
}

void AllocationDiagnosticsTestAccess::seedModuleCache(AllocationSampler &, std::size_t) noexcept {}

bool AllocationDiagnosticsTestAccess::resolveFrameOnce(AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::resolveFrameTwice(AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::resolveMissingFrame(AllocationSampler &) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::exerciseRecordPoolEmpty(AllocationSampler &, void *) noexcept
{
    return false;
}

bool AllocationDiagnosticsTestAccess::exerciseInsertionProbeExhaustion(AllocationSampler &, void *,
                                                                       std::size_t) noexcept
{
    return false;
}

}  // namespace test
#endif

std::uint64_t AllocationSampler::retainedAverageAgeMs() const
{
    return 0;
}
std::uint64_t AllocationSampler::retainedMaximumAgeMs() const
{
    return 0;
}

bool AllocationSampler::running() const
{
    return false;
}

bool AllocationSampler::hooksInstalled() const
{
    return false;
}

bool AllocationSampler::failure(std::string &error) const
{
    error.clear();
    return false;
}

const char *AllocationSampler::backendId() noexcept
{
    return "native-allocation/unsupported";
}

const char *AllocationSampler::backendName() noexcept
{
    return "Unsupported native allocation backend";
}

const char *AllocationSampler::resolvedBackendName() const noexcept
{
    return backendName();
}

const std::vector<AllocationHookCapability> &AllocationSampler::hookCapabilities() const
{
    return impl_->capabilities;
}

std::size_t AllocationSampler::hookTargetCount() const
{
    return 0;
}

// NOLINTEND(readability-convert-member-functions-to-static)

}  // namespace spark
