#include "platform/endstone/papi_integration.h"

#include <utility>

#include "application/placeholder/spark_placeholder_resolver.h"

namespace spark::endstone_adapter {

SparkPlaceholderExpansion::SparkPlaceholderExpansion(const StatisticsService &statistics, std::string version)
    : statistics_(statistics), version_(std::move(version))
{
}

std::string SparkPlaceholderExpansion::getIdentifier() const
{
    return "spark";
}

std::string SparkPlaceholderExpansion::getAuthor() const
{
    return "ReallocAll";
}

std::string SparkPlaceholderExpansion::getVersion() const
{
    return version_;
}

std::string SparkPlaceholderExpansion::getName() const
{
    return "spark";
}

std::optional<std::string> SparkPlaceholderExpansion::onRequest(const endstone::OfflinePlayer * /*player*/,
                                                                std::string_view params) noexcept
{
    try {
        return resolveSparkPlaceholder(*this, params);
    }
    catch (...) {
        return std::nullopt;
    }
}

RollingValue SparkPlaceholderExpansion::placeholderTps(std::int64_t window_ms) const
{
    return statistics_.placeholderTps(window_ms);
}

DistributionValues SparkPlaceholderExpansion::placeholderTickDuration(std::size_t max_samples) const
{
    return statistics_.placeholderTickDuration(max_samples);
}

RollingValue SparkPlaceholderExpansion::placeholderCpu(std::int64_t window_ms, bool process) const
{
    return statistics_.placeholderCpu(window_ms, process);
}

PapiRegistrationResult PapiIntegration::enable(endstone::Plugin &owner, std::shared_ptr<papi::PlaceholderAPI> api,
                                               const StatisticsService &statistics, std::string version) noexcept
{
    try {
        if (api_ == api && api_ && api_->isActive()) {
            return PapiRegistrationResult::AlreadyRegistered;
        }
        if (api_) {
            disable(owner);
        }
        if (!api || !api->isActive()) {
            return PapiRegistrationResult::Unavailable;
        }

        auto expansion = std::make_shared<SparkPlaceholderExpansion>(statistics, std::move(version));
        if (!api->registerExpansion(owner, expansion)) {
            return PapiRegistrationResult::Rejected;
        }
        api_ = std::move(api);
        expansion_ = std::move(expansion);
        return PapiRegistrationResult::Registered;
    }
    catch (...) {
        return PapiRegistrationResult::Rejected;
    }
}

bool PapiIntegration::disable(endstone::Plugin &owner) noexcept
{
    bool removed = false;
    try {
        if (api_ && api_->isActive()) {
            removed = api_->unregisterExpansion(owner, "spark");
        }
    }
    catch (...) {
        removed = false;
    }
    expansion_.reset();
    api_.reset();
    return removed;
}

}  // namespace spark::endstone_adapter
