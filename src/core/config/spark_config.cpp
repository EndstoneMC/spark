#include "core/config/spark_config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <toml.hpp>
#include <type_traits>

#include <curl/curl.h>

#include "core/config/spark_config_environment.h"
#include "core/metadata/server_properties.h"
#include "core/util/state_file.h"

namespace spark {

namespace {

constexpr std::int64_t KMaxBackgroundProfilerIntervalMs = 1000;
constexpr std::int64_t KMaxAutoProfilerIntervalMs = 1000;
constexpr std::int64_t KMaxAutoProfilerDurationSeconds = 10 * 60;
constexpr std::size_t KMaxConfigFileBytes = 1U * 1024U * 1024U;
constexpr std::size_t KMaxAdditionalServerPropertyKeys = 64;
constexpr std::size_t KMaxServerPropertyKeyLength = 128;

struct ConfigValues {
    std::string viewer_url;
    std::string bytebin_url;
    std::string bytesocks_host;
    bool background_profiler_enabled;
    std::int64_t background_profiler_interval;
    std::string background_profiler_thread_grouper;
    std::string background_profiler_thread_dumper;
    bool auto_profiler_enabled;
    double auto_profiler_mspt_threshold;
    std::int64_t auto_profiler_trigger_duration_seconds;
    std::int64_t auto_profiler_duration_seconds;
    std::int64_t auto_profiler_interval;
    std::int64_t auto_profiler_cooldown_seconds;
    std::string auto_profiler_thread_grouper;
    std::string auto_profiler_thread_dumper;
    bool allocation_rate_metrics_enabled;
    std::vector<std::string> server_properties_additional_keys;
    bool disable_response_broadcast;
};

bool hasInvalidEndpointCharacters(std::string_view value)
{
    return std::ranges::any_of(value, [](unsigned char ch) { return ch <= 0x20 || ch == 0x7f; });
}

bool hasUrlPart(CURLU *url, CURLUPart part)
{
    char *value = nullptr;
    const CURLUcode result = curl_url_get(url, part, &value, 0);
    if (value != nullptr) {
        curl_free(value);
    }
    return result == CURLUE_OK;
}

bool validHttpBaseUrl(std::string_view value)
{
    if (value.empty() || hasInvalidEndpointCharacters(value)) {
        return false;
    }
    const auto scheme_end = value.find("://");
    if (scheme_end == std::string_view::npos || scheme_end + 3 >= value.size() || value[scheme_end + 3] == '/') {
        return false;
    }
    CURLU *url = curl_url();
    if (url == nullptr) {
        return false;
    }
    const std::string text(value);
    const CURLUcode parsed = curl_url_set(url, CURLUPART_URL, text.c_str(), 0);
    char *scheme = nullptr;
    char *host = nullptr;
    const bool valid = parsed == CURLUE_OK && curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                       curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK && host != nullptr && host[0] != '\0' &&
                       (std::string_view(scheme) == "http" || std::string_view(scheme) == "https") &&
                       !hasUrlPart(url, CURLUPART_USER) && !hasUrlPart(url, CURLUPART_PASSWORD) &&
                       !hasUrlPart(url, CURLUPART_QUERY) && !hasUrlPart(url, CURLUPART_FRAGMENT);
    if (scheme != nullptr) {
        curl_free(scheme);
    }
    if (host != nullptr) {
        curl_free(host);
    }
    curl_url_cleanup(url);
    return valid;
}

bool validWebSocketAuthority(std::string_view value)
{
    if (value.empty() || hasInvalidEndpointCharacters(value) || value.find_first_of("/?#@") != std::string_view::npos) {
        return false;
    }
    return validHttpBaseUrl("https://" + std::string(value) + "/");
}

std::string escapeString(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char ch : s) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            }
            else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

std::string trimConfigToken(std::string_view value)
{
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

bool validServerPropertyKey(std::string_view key)
{
    if (key.empty() || key.size() > KMaxServerPropertyKeyLength) {
        return false;
    }
    return std::ranges::all_of(key, [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
               ch == '_' || ch == '.';
    });
}

bool parseAdditionalServerPropertyKeys(std::string_view text, std::vector<std::string> &keys, std::string &error)
{
    keys.clear();
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        std::string key = trimConfigToken(text.substr(start, end - start));
        if (!key.empty()) {
            if (!validServerPropertyKey(key)) {
                error = "Invalid serverPropertiesAdditionalKeys entry - using defaults";
                return false;
            }
            if (std::ranges::find(keys, key) == keys.end()) {
                keys.push_back(std::move(key));
                if (keys.size() > KMaxAdditionalServerPropertyKeys) {
                    error = "Too many serverPropertiesAdditionalKeys entries - using defaults";
                    return false;
                }
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return true;
}

std::optional<double> numericConfigValue(const toml::parse_result &result, const char *key)
{
    if (const auto floating = result[key].value<double>()) {
        return floating;
    }
    if (const auto integer = result[key].value<std::int64_t>()) {
        return static_cast<double>(*integer);
    }
    return std::nullopt;
}

std::string joinAdditionalServerPropertyKeys(const std::vector<std::string> &keys)
{
    std::string result;
    for (const auto &key : keys) {
        if (!result.empty()) {
            result += ',';
        }
        result += key;
    }
    return result;
}

void applyEnvironmentOverrides(ConfigValues &values)
{
    const SparkConfigEnvironment environment = readSparkConfigEnvironment();
    if (environment.viewer_url) {
        values.viewer_url = *environment.viewer_url;
    }
    if (environment.bytebin_url) {
        values.bytebin_url = *environment.bytebin_url;
    }
    if (environment.bytesocks_host) {
        values.bytesocks_host = *environment.bytesocks_host;
    }
    if (environment.background_profiler_enabled) {
        values.background_profiler_enabled = *environment.background_profiler_enabled;
    }
    if (environment.background_profiler_interval) {
        values.background_profiler_interval = *environment.background_profiler_interval;
    }
    if (environment.background_profiler_thread_grouper) {
        values.background_profiler_thread_grouper = *environment.background_profiler_thread_grouper;
    }
    if (environment.background_profiler_thread_dumper) {
        values.background_profiler_thread_dumper = *environment.background_profiler_thread_dumper;
    }
    if (environment.allocation_rate_metrics_enabled) {
        values.allocation_rate_metrics_enabled = *environment.allocation_rate_metrics_enabled;
    }
    if (environment.disable_response_broadcast) {
        values.disable_response_broadcast = *environment.disable_response_broadcast;
    }
}

bool validateConfigValues(ConfigValues &values, std::string &error)
{
    if (!validHttpBaseUrl(values.viewer_url) || !validHttpBaseUrl(values.bytebin_url) ||
        !validWebSocketAuthority(values.bytesocks_host)) {
        error = "Invalid Spark network endpoint - using defaults";
        return false;
    }
    if (values.background_profiler_interval < 1 ||
        values.background_profiler_interval > KMaxBackgroundProfilerIntervalMs ||
        values.background_profiler_interval > std::numeric_limits<int>::max()) {
        error = "backgroundProfilerInterval must be between 1 and 1000 milliseconds - using defaults";
        return false;
    }
    if (values.background_profiler_thread_grouper != "by-pool" &&
        values.background_profiler_thread_grouper != "by-name" &&
        values.background_profiler_thread_grouper != "as-one") {
        error = "Invalid backgroundProfilerThreadGrouper - using defaults";
        return false;
    }
    if (values.background_profiler_thread_dumper != "default" && values.background_profiler_thread_dumper != "all") {
        error = "Invalid backgroundProfilerThreadDumper - using defaults";
        return false;
    }
    if (!std::isfinite(values.auto_profiler_mspt_threshold) || values.auto_profiler_mspt_threshold <= 0.0) {
        error = "autoProfilerMsptThreshold must be a positive finite number - using defaults";
        return false;
    }
    if (values.auto_profiler_trigger_duration_seconds < 1 ||
        values.auto_profiler_trigger_duration_seconds > std::numeric_limits<int>::max()) {
        error = "autoProfilerTriggerDuration must be a positive number of seconds - using defaults";
        return false;
    }
    if (values.auto_profiler_duration_seconds < 1 ||
        values.auto_profiler_duration_seconds > KMaxAutoProfilerDurationSeconds) {
        error = "autoProfilerDuration must be between 1 and 600 seconds - using defaults";
        return false;
    }
    if (values.auto_profiler_interval < 1 || values.auto_profiler_interval > KMaxAutoProfilerIntervalMs) {
        error = "autoProfilerInterval must be between 1 and 1000 milliseconds - using defaults";
        return false;
    }
    if (values.auto_profiler_cooldown_seconds < 0 ||
        values.auto_profiler_cooldown_seconds > std::numeric_limits<int>::max()) {
        error = "autoProfilerCooldown must be a non-negative number of seconds - using defaults";
        return false;
    }
    if (values.auto_profiler_thread_grouper != "by-pool" && values.auto_profiler_thread_grouper != "by-name" &&
        values.auto_profiler_thread_grouper != "as-one") {
        error = "Invalid autoProfilerThreadGrouper - using defaults";
        return false;
    }
    if (values.auto_profiler_thread_dumper != "default" && values.auto_profiler_thread_dumper != "all") {
        error = "Invalid autoProfilerThreadDumper - using defaults";
        return false;
    }

    if (values.viewer_url.back() != '/') {
        values.viewer_url.push_back('/');
    }
    if (values.bytebin_url.back() != '/') {
        values.bytebin_url.push_back('/');
    }
    return true;
}

ConfigValues currentConfigValues(const SparkConfig &config)
{
    return {.viewer_url = config.viewer_url,
            .bytebin_url = config.bytebin_url,
            .bytesocks_host = config.bytesocks_host,
            .background_profiler_enabled = config.background_profiler_enabled,
            .background_profiler_interval = config.background_profiler_interval,
            .background_profiler_thread_grouper = config.background_profiler_thread_grouper,
            .background_profiler_thread_dumper = config.background_profiler_thread_dumper,
            .auto_profiler_enabled = config.auto_profiler_enabled,
            .auto_profiler_mspt_threshold = config.auto_profiler_mspt_threshold,
            .auto_profiler_trigger_duration_seconds = config.auto_profiler_trigger_duration_seconds,
            .auto_profiler_duration_seconds = config.auto_profiler_duration_seconds,
            .auto_profiler_interval = config.auto_profiler_interval,
            .auto_profiler_cooldown_seconds = config.auto_profiler_cooldown_seconds,
            .auto_profiler_thread_grouper = config.auto_profiler_thread_grouper,
            .auto_profiler_thread_dumper = config.auto_profiler_thread_dumper,
            .allocation_rate_metrics_enabled = config.allocation_rate_metrics_enabled,
            .server_properties_additional_keys = config.server_properties_additional_keys,
            .disable_response_broadcast = config.disable_response_broadcast};
}

void commitConfigValues(SparkConfig &config, ConfigValues values)
{
    config.viewer_url = std::move(values.viewer_url);
    config.bytebin_url = std::move(values.bytebin_url);
    config.bytesocks_host = std::move(values.bytesocks_host);
    config.background_profiler_enabled = values.background_profiler_enabled;
    config.background_profiler_interval = static_cast<int>(values.background_profiler_interval);
    config.background_profiler_thread_grouper = std::move(values.background_profiler_thread_grouper);
    config.background_profiler_thread_dumper = std::move(values.background_profiler_thread_dumper);
    config.auto_profiler_enabled = values.auto_profiler_enabled;
    config.auto_profiler_mspt_threshold = values.auto_profiler_mspt_threshold;
    config.auto_profiler_trigger_duration_seconds = static_cast<int>(values.auto_profiler_trigger_duration_seconds);
    config.auto_profiler_duration_seconds = static_cast<int>(values.auto_profiler_duration_seconds);
    config.auto_profiler_interval = static_cast<int>(values.auto_profiler_interval);
    config.auto_profiler_cooldown_seconds = static_cast<int>(values.auto_profiler_cooldown_seconds);
    config.auto_profiler_thread_grouper = std::move(values.auto_profiler_thread_grouper);
    config.auto_profiler_thread_dumper = std::move(values.auto_profiler_thread_dumper);
    config.allocation_rate_metrics_enabled = values.allocation_rate_metrics_enabled;
    config.server_properties_additional_keys = std::move(values.server_properties_additional_keys);
    config.disable_response_broadcast = values.disable_response_broadcast;
    setAdditionalSafeServerPropertyKeys(config.server_properties_additional_keys);
}

}  // namespace

SparkConfig::SparkConfig(std::filesystem::path file) : file_(std::move(file)) {}

bool SparkConfig::load()
{
    last_error_.clear();

    std::string text;
    if (!readStateFile(file_, KMaxConfigFileBytes, text, last_error_)) {
        return false;
    }

    toml::parse_result result;
    try {
        result = toml::parse(text);
    }
    catch (const toml::parse_error &) {
        last_error_ = "Malformed TOML in config file - using defaults";
        return false;
    }

    ConfigValues values{
        .viewer_url = result["viewerUrl"].value<std::string>().value_or(this->viewer_url),
        .bytebin_url = result["bytebinUrl"].value<std::string>().value_or(this->bytebin_url),
        .bytesocks_host = result["bytesocksHost"].value<std::string>().value_or(this->bytesocks_host),
        .background_profiler_enabled = result["backgroundProfiler"].value<bool>().value_or(background_profiler_enabled),
        .background_profiler_interval =
            result["backgroundProfilerInterval"].value<std::int64_t>().value_or(background_profiler_interval),
        .background_profiler_thread_grouper =
            result["backgroundProfilerThreadGrouper"].value<std::string>().value_or(background_profiler_thread_grouper),
        .background_profiler_thread_dumper =
            result["backgroundProfilerThreadDumper"].value<std::string>().value_or(background_profiler_thread_dumper),
        .auto_profiler_enabled = result["autoProfiler"].value<bool>().value_or(auto_profiler_enabled),
        .auto_profiler_mspt_threshold =
            numericConfigValue(result, "autoProfilerMsptThreshold").value_or(auto_profiler_mspt_threshold),
        .auto_profiler_trigger_duration_seconds = result["autoProfilerTriggerDuration"].value<std::int64_t>().value_or(
            auto_profiler_trigger_duration_seconds),
        .auto_profiler_duration_seconds =
            result["autoProfilerDuration"].value<std::int64_t>().value_or(auto_profiler_duration_seconds),
        .auto_profiler_interval = result["autoProfilerInterval"].value<std::int64_t>().value_or(auto_profiler_interval),
        .auto_profiler_cooldown_seconds =
            result["autoProfilerCooldown"].value<std::int64_t>().value_or(auto_profiler_cooldown_seconds),
        .auto_profiler_thread_grouper =
            result["autoProfilerThreadGrouper"].value<std::string>().value_or(auto_profiler_thread_grouper),
        .auto_profiler_thread_dumper =
            result["autoProfilerThreadDumper"].value<std::string>().value_or(auto_profiler_thread_dumper),
        .allocation_rate_metrics_enabled =
            result["allocationRateMetrics"].value<bool>().value_or(allocation_rate_metrics_enabled),
        .server_properties_additional_keys = server_properties_additional_keys,
        .disable_response_broadcast =
            result["disableResponseBroadcast"].value<bool>().value_or(disable_response_broadcast)};

    const auto invalid_type = [&result](std::string_view key, const auto &tag) {
        using Value = std::remove_cvref_t<decltype(tag)>;
        return result[key] && !result[key].value<Value>();
    };
    const auto invalid_number = [&result](const char *key) {
        return result[key] && !numericConfigValue(result, key);
    };
    if (invalid_type("viewerUrl", std::string{}) || invalid_type("bytebinUrl", std::string{}) ||
        invalid_type("bytesocksHost", std::string{}) || invalid_type("backgroundProfiler", bool{}) ||
        invalid_type("backgroundProfilerThreadGrouper", std::string{}) ||
        invalid_type("backgroundProfilerThreadDumper", std::string{}) || invalid_type("autoProfiler", bool{}) ||
        invalid_number("autoProfilerMsptThreshold") || invalid_type("autoProfilerTriggerDuration", std::int64_t{}) ||
        invalid_type("autoProfilerDuration", std::int64_t{}) || invalid_type("autoProfilerInterval", std::int64_t{}) ||
        invalid_type("autoProfilerCooldown", std::int64_t{}) ||
        invalid_type("autoProfilerThreadGrouper", std::string{}) ||
        invalid_type("autoProfilerThreadDumper", std::string{}) || invalid_type("allocationRateMetrics", bool{}) ||
        invalid_type("serverPropertiesAdditionalKeys", std::string{}) ||
        invalid_type("disableResponseBroadcast", bool{}) ||
        invalid_type("backgroundProfilerInterval", std::int64_t{})) {
        last_error_ = "Invalid type for a spark configuration value - using defaults";
        return false;
    }

    if (const auto additional_keys = result["serverPropertiesAdditionalKeys"].value<std::string>(); additional_keys) {
        if (!parseAdditionalServerPropertyKeys(*additional_keys, values.server_properties_additional_keys,
                                               last_error_)) {
            return false;
        }
    }

    applyEnvironmentOverrides(values);
    if (!validateConfigValues(values, last_error_)) {
        return false;
    }
    commitConfigValues(*this, std::move(values));

    return true;
}

bool SparkConfig::loadOrCreate()
{
    std::error_code error;
    const bool exists = std::filesystem::exists(file_, error);
    if (error) {
        last_error_ = "Unable to inspect config file: " + error.message();
        return false;
    }
    if (exists) {
        return load();
    }
    if (!save()) {
        return false;
    }

    auto values = currentConfigValues(*this);
    applyEnvironmentOverrides(values);
    if (!validateConfigValues(values, last_error_)) {
        return false;
    }
    commitConfigValues(*this, std::move(values));
    return true;
}

void SparkConfig::writeTemplate(std::ostream &out) const
{
    out << "# spark configuration file\n";
    out << "# https://spark.lucko.me/docs/Configuration\n";
    out << "\n";
    out << "# HTTP(S) base URL of the spark viewer\n";
    out << "viewerUrl = \"" << escapeString(viewer_url) << "\"\n";
    out << "\n";
    out << "# HTTP(S) bytebin upload endpoint\n";
    out << "bytebinUrl = \"" << escapeString(bytebin_url) << "\"\n";
    out << "\n";
    out << "# Bytesocks host[:port] without a scheme or path\n";
    out << "bytesocksHost = \"" << escapeString(bytesocks_host) << "\"\n";
    out << "\n";
    out << "# Whether the background profiler should run\n";
    out << "backgroundProfiler = " << (background_profiler_enabled ? "true" : "false") << "\n";
    out << "\n";
    out << "# Background sampling interval in milliseconds (1-1000)\n";
    out << "backgroundProfilerInterval = " << background_profiler_interval << "\n";
    out << "\n";
    out << "# Thread grouping: by-pool, by-name, or as-one\n";
    out << "backgroundProfilerThreadGrouper = \"" << escapeString(background_profiler_thread_grouper) << "\"\n";
    out << "\n";
    out << "# Thread selection: default or all\n";
    out << "backgroundProfilerThreadDumper = \"" << escapeString(background_profiler_thread_dumper) << "\"\n";
    out << "\n";
    out << "# Automatically start a bounded execution profile after sustained high MSPT\n";
    out << "autoProfiler = " << (auto_profiler_enabled ? "true" : "false") << "\n";
    out << "\n";
    out << "# Trigger threshold in milliseconds per tick\n";
    out << "autoProfilerMsptThreshold = " << auto_profiler_mspt_threshold << "\n";
    out << "\n";
    out << "# Threshold must remain exceeded for this many seconds\n";
    out << "autoProfilerTriggerDuration = " << auto_profiler_trigger_duration_seconds << "\n";
    out << "\n";
    out << "# Automatic profile duration in seconds (1-600)\n";
    out << "autoProfilerDuration = " << auto_profiler_duration_seconds << "\n";
    out << "\n";
    out << "# Automatic execution sampling interval in milliseconds (1-1000)\n";
    out << "autoProfilerInterval = " << auto_profiler_interval << "\n";
    out << "\n";
    out << "# Delay after an automatic profile finishes before another may trigger\n";
    out << "autoProfilerCooldown = " << auto_profiler_cooldown_seconds << "\n";
    out << "\n";
    out << "# Automatic profile thread grouping: by-pool, by-name, or as-one\n";
    out << "autoProfilerThreadGrouper = \"" << escapeString(auto_profiler_thread_grouper) << "\"\n";
    out << "\n";
    out << "# Automatic profile thread selection: default or all\n";
    out << "autoProfilerThreadDumper = \"" << escapeString(auto_profiler_thread_dumper) << "\"\n";
    out << "\n";
    out << "# Track native process allocation throughput using the existing allocator hooks\n";
    out << "# Enabled by default after Linux/Windows real-BDS overhead validation\n";
    out << "allocationRateMetrics = " << (allocation_rate_metrics_enabled ? "true" : "false") << "\n";
    out << "\n";
    out << "# Comma-separated server.properties keys explicitly reviewed as safe to upload\n";
    out << "# Known-sensitive names (seeds, credentials, debugger endpoints) remain blocked\n";
    out << "serverPropertiesAdditionalKeys = \""
        << escapeString(joinAdditionalServerPropertyKeys(server_properties_additional_keys)) << "\"\n";
    out << "\n";
    out << "# Restrict result notifications to the originating player\n";
    out << "disableResponseBroadcast = " << (disable_response_broadcast ? "true" : "false") << "\n";
}

bool SparkConfig::save() const
{
    last_error_.clear();

    std::ostringstream ss;
    writeTemplate(ss);

    const std::string text = ss.str();
    return writeStateFileAtomically(file_, text, last_error_);
}

}  // namespace spark
