#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <endstone/plugin/plugin_description.h>

#include "platform/endstone/papi_integration.h"

namespace {

class FakePlugin final : public endstone::Plugin {
public:
    [[nodiscard]] const endstone::PluginDescription &getDescription() const override { return description_; }

private:
    endstone::PluginDescription description_{"spark", "0.6.0"};
};

class FakePlaceholderApi final : public papi::PlaceholderAPI {
public:
    [[nodiscard]] bool isActive() const noexcept override { return active; }

    [[nodiscard]] std::string setPlaceholders(const endstone::OfflinePlayer *player,
                                              std::string_view text) const override
    {
        if (!active || !expansion || text != "{spark:unknown}") {
            return std::string(text);
        }
        auto value = expansion->onRequest(player, "unknown");
        return value.value_or(std::string(text));
    }

    [[nodiscard]] std::string setRelationalPlaceholders(const endstone::Player &, const endstone::Player &,
                                                        std::string_view text) const override
    {
        return std::string(text);
    }

    [[nodiscard]] bool containsPlaceholders(std::string_view text) const noexcept override
    {
        return text.find('{') != std::string_view::npos && text.find('}') != std::string_view::npos;
    }

    [[nodiscard]] bool isRegistered(std::string_view identifier) const override
    {
        return expansion && identifier == "spark";
    }

    [[nodiscard]] std::vector<std::string> getRegisteredIdentifiers() const override
    {
        return expansion ? std::vector<std::string>{"spark"} : std::vector<std::string>{};
    }

    [[nodiscard]] std::vector<papi::ExpansionInfo> getExpansions() const override { return {}; }

    bool registerExpansion(endstone::Plugin &owner, std::shared_ptr<papi::PlaceholderExpansion> candidate) override
    {
        ++register_calls;
        if (!active || reject_registration || expansion) {
            return false;
        }
        registered_owner = &owner;
        expansion = std::move(candidate);
        return true;
    }

    bool unregisterExpansion(endstone::Plugin &owner, std::string_view identifier) override
    {
        ++unregister_calls;
        if (!active || registered_owner != &owner || identifier != "spark" || !expansion) {
            return false;
        }
        expansion.reset();
        registered_owner = nullptr;
        return true;
    }

    std::size_t unregisterExpansions(endstone::Plugin &owner) override
    {
        return unregisterExpansion(owner, "spark") ? 1 : 0;
    }

    bool active = true;
    bool reject_registration = false;
    int register_calls = 0;
    int unregister_calls = 0;
    endstone::Plugin *registered_owner = nullptr;
    std::shared_ptr<papi::PlaceholderExpansion> expansion;
};

}  // namespace

int main()
{
    FakePlugin owner;
    spark::StatisticsService statistics;
    spark::endstone_adapter::PapiIntegration integration;

    assert(integration.enable(owner, nullptr, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Unavailable);
    assert(!integration.registered());

    auto inactive = std::make_shared<FakePlaceholderApi>();
    inactive->active = false;
    assert(integration.enable(owner, inactive, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Unavailable);

    auto first = std::make_shared<FakePlaceholderApi>();
    assert(integration.enable(owner, first, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Registered);
    assert(integration.registered());
    assert(first->register_calls == 1);
    assert(first->expansion);
    assert(first->expansion->getIdentifier() == "spark");
    assert(first->expansion->getName() == "spark");
    assert(first->expansion->getAuthor() == "ReallocAll");
    assert(first->expansion->getVersion() == "0.6.0");
    assert(!first->expansion->supportsRelationalPlaceholders());
    assert(!first->expansion->supportsPlayerCleanup());
    assert(!first->expansion->onRequest(nullptr, "unknown").has_value());
    assert(first->setPlaceholders(nullptr, "{spark:unknown}") == "{spark:unknown}");

    assert(integration.enable(owner, first, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::AlreadyRegistered);
    assert(first->register_calls == 1);
    assert(integration.disable(owner));
    assert(!integration.registered());
    assert(first->unregister_calls == 1);
    assert(!first->expansion);

    auto rejected = std::make_shared<FakePlaceholderApi>();
    rejected->reject_registration = true;
    assert(integration.enable(owner, rejected, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Rejected);
    assert(!integration.registered());

    assert(integration.enable(owner, first, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Registered);
    first->active = false;
    assert(first->setPlaceholders(nullptr, "{spark:unknown}") == "{spark:unknown}");

    auto replacement = std::make_shared<FakePlaceholderApi>();
    assert(integration.enable(owner, replacement, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Registered);
    assert(first->unregister_calls == 1);
    assert(replacement->register_calls == 1);
    assert(integration.disable(owner));
    assert(replacement->unregister_calls == 1);
}
