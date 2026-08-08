#ifndef SPARK_CORE_METADATA_SERVER_PROPERTIES_H
#define SPARK_CORE_METADATA_SERVER_PROPERTIES_H

#include <filesystem>
#include <map>
#include <string>

namespace spark {

// Parses server.properties with a strict allowlist; unsafe keys are silently dropped.
std::map<std::string, std::string> parseServerProperties(const std::filesystem::path &file);

// Serializes properties as a JSON object string matching upstream spark's server_configurations.
std::string serverPropertiesToJsonString(const std::map<std::string, std::string> &properties);

}  // namespace spark

#endif  // SPARK_CORE_METADATA_SERVER_PROPERTIES_H
