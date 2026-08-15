#ifndef SPARK_APPLICATION_PLACEHOLDER_SPARK_PLACEHOLDER_RESOLVER_H
#define SPARK_APPLICATION_PLACEHOLDER_SPARK_PLACEHOLDER_RESOLVER_H

#include <optional>
#include <string>
#include <string_view>

#include "core/stats/statistics_service.h"

namespace spark {

class SparkPlaceholderStatistics {
public:
    virtual ~SparkPlaceholderStatistics() = default;
    [[nodiscard]] virtual RollingValue placeholderTps(std::int64_t window_ms) const = 0;
    [[nodiscard]] virtual DistributionValues placeholderTickDuration(std::size_t max_samples) const = 0;
    [[nodiscard]] virtual RollingValue placeholderCpu(std::int64_t window_ms, bool process) const = 0;
};

std::optional<std::string> resolveSparkPlaceholder(const SparkPlaceholderStatistics &statistics,
                                                   std::string_view params);

}  // namespace spark

#endif  // SPARK_APPLICATION_PLACEHOLDER_SPARK_PLACEHOLDER_RESOLVER_H
