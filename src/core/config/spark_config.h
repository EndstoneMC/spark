#ifndef SPARK_CORE_CONFIG_SPARK_CONFIG_H
#define SPARK_CORE_CONFIG_SPARK_CONFIG_H

#include <filesystem>
#include <string>
#include <vector>

namespace spark {

// Persistent Spark configuration loaded from config.toml in the plugin data
// directory.  All fields have safe defaults; a missing or malformed file
// produces a warning and falls back to those defaults.  Unknown keys are
// silently ignored so that future config additions do not break older builds.
//
// The file is user-owned: save() is only for first-time default creation.
// Runtime mutations (e.g. trust-viewer) go through TrustedViewersState, never
// through this class.
class SparkConfig {
public:
    explicit SparkConfig(std::filesystem::path file);

    // Loads config.toml.  If config.toml does not exist but a legacy
    // config.json is present, migrates: reads JSON values, writes config.toml,
    // renames config.json to config.json.bak.  If migrated_trusted_keys is
    // non-null and trustedKeys was present in the legacy config.json, fills it
    // with those keys so the caller can seed TrustedViewersState.
    bool load(std::vector<std::string> *migrated_trusted_keys = nullptr);

    // Writes the default config.toml template with explanatory comments.
    // Only for first-time creation; never called during normal operation.
    bool save() const;

    // --- URL endpoints ---
    std::string viewer_url = "https://spark.lucko.me/";
    std::string bytebin_url = "https://spark-usercontent.lucko.me/";
    std::string bytesocks_host = "spark-usersockets.lucko.me";

    // --- Background profiler ---
    bool background_profiler_enabled = true;
    int background_profiler_interval = 10;
    std::string background_profiler_thread_grouper = "by-pool";
    std::string background_profiler_thread_dumper = "default";

    // --- Response behaviour ---
    bool disable_response_broadcast = false;

    // Returns the last load/save error message, or empty if none.
    const std::string &lastError() const { return last_error_; }

private:
    bool loadToml();
    bool migrateFromJson(std::vector<std::string> *migrated_trusted_keys);
    void writeTemplate(std::ostream &out) const;

    std::filesystem::path file_;
    mutable std::string last_error_;
};

}  // namespace spark

#endif  // SPARK_CORE_CONFIG_SPARK_CONFIG_H
