#ifndef SPARK_PLATFORM_ENDSTONE_NATIVE_PLUGIN_ATTRIBUTION_H
#define SPARK_PLATFORM_ENDSTONE_NATIVE_PLUGIN_ATTRIBUTION_H

#include <cstdint>
#include <optional>
#include <string>

namespace endstone {
class Plugin;
}

namespace spark::endstone_adapter {

struct NativePluginModuleIdentity {
    std::uintptr_t module_base = 0;
    std::string module_path;
};

std::optional<NativePluginModuleIdentity> identifyNativePluginModule(const ::endstone::Plugin &plugin);

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_NATIVE_PLUGIN_ATTRIBUTION_H
