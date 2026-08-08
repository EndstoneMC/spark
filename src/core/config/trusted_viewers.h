#ifndef SPARK_CORE_CONFIG_TRUSTED_VIEWERS_H
#define SPARK_CORE_CONFIG_TRUSTED_VIEWERS_H

#include <filesystem>
#include <string>
#include <vector>

namespace spark {

// Manages trusted viewer public keys, stored separately from config.toml
// so that trust-viewer never rewrites the user-owned configuration file.
class TrustedViewersState {
public:
    explicit TrustedViewersState(std::filesystem::path file);

    // Loads keys from the JSON file.  On any error, keys keep their current
    // (empty) state and the method returns false.
    bool load();

    // Saves keys to the file as a pretty-printed JSON array.
    bool save() const;

    bool contains(const std::string &b64_key) const;
    void add(const std::string &b64_key);

    const std::vector<std::string> &keys() const { return keys_; }
    const std::string &lastError() const { return last_error_; }

private:
    std::filesystem::path file_;
    std::vector<std::string> keys_;
    mutable std::string last_error_;
};

}  // namespace spark

#endif  // SPARK_CORE_CONFIG_TRUSTED_VIEWERS_H
