#include "native/alloc/allocation_sampler.h"

#include <memory>
#include <utility>

namespace spark {

struct AllocationSampler::Impl {
    CallTree tree;
    std::map<std::uint64_t, ThreadCallTree> thread_trees;
    ModuleTable modules;
    std::map<std::int32_t, WindowTickStats> windows;
    std::vector<AllocationHookCapability> capabilities;
};

AllocationSampler::AllocationSampler() : impl_(std::make_unique<Impl>())
{
}

AllocationSampler::~AllocationSampler() = default;

bool AllocationSampler::start(const AllocationSamplerConfig &, std::string &error)
{
    error =
        "native allocation profiling is supported only on Windows x64 and Linux x86-64";
    return false;
}

bool AllocationSampler::stop(std::string &error)
{
    error.clear();
    return true;
}

bool AllocationSampler::shutdown(std::string &error)
{
    error.clear();
    return true;
}

void AllocationSampler::onTick(double)
{
}

void AllocationSampler::setRecoverySink(RecoverySink *)
{
}

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

std::uint64_t AllocationSampler::hookCalls() const { return 0; }
std::uint64_t AllocationSampler::successfulAllocationCalls() const { return 0; }

std::uint64_t AllocationSampler::sampleCount() const
{
    return 0;
}

std::uint64_t AllocationSampler::samplingPoints() const { return 0; }

std::uint64_t AllocationSampler::sampledBytes() const
{
    return 0;
}
std::uint64_t AllocationSampler::filteredSamples() const { return 0; }
std::uint64_t AllocationSampler::threadNameFailures() const { return 0; }
std::uint64_t AllocationSampler::threadIdentityCacheDrops() const { return 0; }

std::uint64_t AllocationSampler::observedBytes() const
{
    return 0;
}

std::uint64_t AllocationSampler::droppedSamples() const
{
    return 0;
}

std::uint64_t AllocationSampler::enqueuedSamples() const { return 0; }
std::uint64_t AllocationSampler::droppedEvents() const { return 0; }
std::uint64_t AllocationSampler::droppedTickEvents() const { return 0; }
std::uint64_t AllocationSampler::tickEventCapacity() const { return 0; }
std::uint64_t AllocationSampler::eventQueueHighWaterMark() const { return 0; }
std::uint64_t AllocationSampler::eventQueueCapacity() const { return 0; }
std::uint64_t AllocationSampler::freedSamples() const { return 0; }
std::uint64_t AllocationSampler::freedBytes() const { return 0; }
std::uint64_t AllocationSampler::liveSamples() const { return 0; }
std::uint64_t AllocationSampler::liveBytes() const { return 0; }
std::uint64_t AllocationSampler::peakLiveSamples() const { return 0; }
std::uint64_t AllocationSampler::liveIndexCapacity() const { return 0; }
std::uint64_t AllocationSampler::sampledThreadCount() const { return 0; }
std::uint64_t AllocationSampler::threadRootCapacity() const { return 0; }
std::uint64_t AllocationSampler::overflowThreadCount() const { return 0; }
std::uint64_t AllocationSampler::threadStateDrops() const { return 0; }
std::uint64_t AllocationSampler::hookedModuleCount() const { return 0; }
std::uint64_t AllocationSampler::skippedModuleCount() const { return 0; }
std::uint64_t AllocationSampler::failedModuleCount() const { return 0; }
std::uint64_t AllocationSampler::moduleRegistryCount() const { return 0; }
std::uint64_t AllocationSampler::moduleRegistryCapacity() const { return 0; }
std::uint64_t AllocationSampler::profileNodeCapacity() const { return 0; }
bool AllocationSampler::dataIncomplete() const { return false; }
std::uint64_t AllocationSampler::averageLifetimeMs() const { return 0; }
std::uint64_t AllocationSampler::maximumLifetimeMs() const { return 0; }
std::uint64_t AllocationSampler::lifecycleDropped() const { return 0; }
std::uint64_t AllocationSampler::retainedAverageAgeMs() const { return 0; }
std::uint64_t AllocationSampler::retainedMaximumAgeMs() const { return 0; }

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

const std::vector<AllocationHookCapability> &AllocationSampler::hookCapabilities() const
{
    return impl_->capabilities;
}

std::size_t AllocationSampler::hookTargetCount() const
{
    return 0;
}

}  // namespace spark
