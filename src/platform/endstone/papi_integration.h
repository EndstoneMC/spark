#ifndef SPARK_PLATFORM_ENDSTONE_PAPI_INTEGRATION_H
#define SPARK_PLATFORM_ENDSTONE_PAPI_INTEGRATION_H

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <endstone_papi/placeholder_api.h>

#include "application/placeholder/spark_placeholder_resolver.h"
#include "core/stats/statistics_service.h"

namespace spark::endstone_adapter {

class SparkPlaceholderExpansion final : public papi::PlaceholderExpansion, private SparkPlaceholderStatistics {
public:
    SparkPlaceholderExpansion(const StatisticsService &statistics, std::string version);

    [[nodiscard]] std::string getIdentifier() const override;
    [[nodiscard]] std::string getAuthor() const override;
    [[nodiscard]] std::string getVersion() const override;
    [[nodiscard]] std::string getName() const override;
    [[nodiscard]] std::optional<std::string> onRequest(const endstone::OfflinePlayer *player,
                                                       std::string_view params) noexcept override;

private:
    [[nodiscard]] RollingValue placeholderTps(std::int64_t window_ms) const override;
    [[nodiscard]] DistributionValues placeholderTickDuration(std::size_t max_samples) const override;
    [[nodiscard]] RollingValue placeholderCpu(std::int64_t window_ms, bool process) const override;

    const StatisticsService &statistics_;
    std::string version_;
};

enum class PapiRegistrationResult {
    Registered,
    AlreadyRegistered,
    Unavailable,
    Rejected,
};

class PapiIntegration {
public:
    PapiRegistrationResult enable(endstone::Plugin &owner, std::shared_ptr<papi::PlaceholderAPI> api,
                                  const StatisticsService &statistics, std::string version) noexcept;
    bool disable(endstone::Plugin &owner) noexcept;

    [[nodiscard]] bool registered() const noexcept { return api_ != nullptr; }

private:
    std::shared_ptr<papi::PlaceholderAPI> api_;
    std::shared_ptr<SparkPlaceholderExpansion> expansion_;
};

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_PAPI_INTEGRATION_H
