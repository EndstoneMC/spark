#ifndef SPARK_CORE_CONFIG_SPARK_CONFIG_H
#define SPARK_CORE_CONFIG_SPARK_CONFIG_H

#include <filesystem>
#include <string>

namespace spark {

// Persistent TOML configuration with safe defaults. The file is user-owned:
// save() is only for first-time creation; runtime mutations go through TrustedViewersState.
class SparkConfig {
public:
    explicit SparkConfig(std::filesystem::path file);

    // Loads config.toml.  On any error, fields keep their current (default)
    // values and the method returns false.
    bool load();

    // Loads an existing file, or creates the default template when absent.
    // Existing invalid files are never rewritten.
    bool loadOrCreate();

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
    void writeTemplate(std::ostream &out) const;

    std::filesystem::path file_;
    mutable std::string last_error_;
};

}  // namespace spark

#endif  // SPARK_CORE_CONFIG_SPARK_CONFIG_H
