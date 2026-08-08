#ifndef SPARK_NATIVE_SAMPLER_RECOVERY_SINK_H
#define SPARK_NATIVE_SAMPLER_RECOVERY_SINK_H

#include <cstdint>
#include <string_view>

#include "native/sampler/types.h"

namespace spark {

// Abstract interface for journaling profiler data to a crash-safe recovery
// log.  Implemented by RecoveryWriter in core/.  The Sampler's aggregator
// thread calls these methods; they must be non-blocking.
class RecoverySink {
public:
    virtual ~RecoverySink() = default;
    virtual void journalModuleDef(std::uint32_t module_id, std::string_view path) = 0;
    virtual void journalThreadDef(std::uint64_t thread_id, std::uint64_t os_thread_id,
                                  std::string_view name) = 0;
    virtual void journalSample(const Sample &sample) = 0;
    virtual void journalTickEvent(std::uint64_t tick_id, double mspt) = 0;
};

}  // namespace spark

#endif  // SPARK_NATIVE_SAMPLER_RECOVERY_SINK_H
