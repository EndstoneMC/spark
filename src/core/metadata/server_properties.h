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

}  // namespace spark

#endif  // SPARK_CORE_METADATA_SERVER_PROPERTIES_H
