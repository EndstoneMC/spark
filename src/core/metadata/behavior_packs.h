#ifndef ENDSTONE_SPARK_BEHAVIOR_PACKS_H
#define ENDSTONE_SPARK_BEHAVIOR_PACKS_H

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/metadata/platform_metadata.h"

namespace spark {

// Discovers the behavior packs selected by the active Bedrock world.
//
// Endstone does not currently expose the active behavior-pack stack through its
// public API, so this deliberately uses Bedrock's world_behavior_packs.json as a
// compatibility fallback. Installed-but-inactive packs and resource packs are
// never exported. Once Endstone exposes a public active-pack-stack API, the
// Endstone adapter should prefer that runtime view and retain this function only
// as a compatibility fallback for older Endstone versions.
std::vector<DataPackInfo> discoverActiveBehaviorPacks(const std::filesystem::path &server_root,
                                                      std::string_view level_name_hint = {});

}  // namespace spark

#endif  // ENDSTONE_SPARK_BEHAVIOR_PACKS_H
