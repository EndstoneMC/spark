#ifndef ENDSTONE_SPARK_PLATFORM_METADATA_CAPTURE_H
#define ENDSTONE_SPARK_PLATFORM_METADATA_CAPTURE_H

#include <cstdint>
#include <utility>

#include "application/platform_capabilities.h"
#include "core/profiler/profiler.h"

namespace spark {

inline void gatherPlatformServerMetadata(ProfileMetadataProvider &provider, ExportContext &context,
                                          std::int64_t now_ms)
{
    ServerMetadata metadata;
    metadata.endstone_version = std::move(context.endstone_version);
    metadata.minecraft_version = std::move(context.minecraft_version);
    metadata.bds_executable_sha256 = std::move(context.bds_executable_sha256);
    metadata.player_count = context.player_count;
    metadata.online_mode = context.online_mode;
    metadata.uptime_ms = context.uptime_ms;
    metadata.plugins = std::move(context.plugins);
    metadata.server_configurations = std::move(context.server_configurations);

    provider.gatherServerMetadata(metadata, now_ms);

    context.endstone_version = std::move(metadata.endstone_version);
    context.minecraft_version = std::move(metadata.minecraft_version);
    context.bds_executable_sha256 = std::move(metadata.bds_executable_sha256);
    context.player_count = metadata.player_count;
    context.online_mode = metadata.online_mode;
    context.uptime_ms = metadata.uptime_ms;
    context.plugins = std::move(metadata.plugins);
    context.server_configurations = std::move(metadata.server_configurations);
}

}  // namespace spark

#endif  // ENDSTONE_SPARK_PLATFORM_METADATA_CAPTURE_H
