#ifndef SPARK_CORE_METADATA_SERVER_PROPERTIES_H
#define SPARK_CORE_METADATA_SERVER_PROPERTIES_H

#include <filesystem>
#include <map>
#include <string>

namespace spark {

// Parses server.properties with a strict allowlist. Only explicitly safe
// keys are returned; all other keys (including any future sensitive fields)
// are silently dropped. Returns an empty map if the file is missing or
// unreadable.
std::map<std::string, std::string> parseServerProperties(
    const std::filesystem::path &file);

// Serializes a properties map into the JSON object string format expected by
// the spark viewer's server_configurations field. Boolean strings ("true"/
// "false") become JSON booleans, all-digit strings become JSON numbers, and
// everything else becomes a JSON string. This matches the upstream Java
// spark PropertiesConfigParser + Gson.toJsonTree pipeline.
std::string serverPropertiesToJsonString(
    const std::map<std::string, std::string> &properties);

}  // namespace spark

#endif  // SPARK_CORE_METADATA_SERVER_PROPERTIES_H
