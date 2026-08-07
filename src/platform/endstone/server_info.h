#ifndef SPARK_PLATFORM_ENDSTONE_SERVER_INFO_H
#define SPARK_PLATFORM_ENDSTONE_SERVER_INFO_H

#include <cstdint>
#include <string>

namespace spark {
struct ExportContext;
}  // namespace spark

namespace endstone {
class Server;
}  // namespace endstone

namespace spark::endstone_adapter {

// Populates ctx.endstone_version, minecraft_version, bds_executable_sha256,
// player_count, online_mode, uptime_ms, and plugins from the Endstone server.
void gatherServerInfo(ExportContext &ctx, ::endstone::Server &server,
                      const std::string &bds_executable_sha256,
                      std::int64_t now_ms);

// Populates ctx.world with world/chunk/entity info from the loaded level.
void gatherWorldInfo(ExportContext &ctx, ::endstone::Server &server);

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_SERVER_INFO_H
