#include <cstdint>
#include <string_view>

#include "application/platform_capabilities.h"

namespace {

class CompileProvider final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ServerMetadata &, std::int64_t) override {}
    void gatherWorldMetadata(spark::WorldInfo &, std::string_view) override {}
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }
};

}  // namespace

int main()
{
    CompileProvider provider;
    return provider.nativePluginSources().empty() ? 0 : 1;
}
