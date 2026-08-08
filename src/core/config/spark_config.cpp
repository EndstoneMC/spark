#include "core/config/spark_config.h"

#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <toml.hpp>
#include <type_traits>

namespace spark {

namespace {

constexpr std::int64_t KMaxBackgroundProfilerIntervalMs = 1000;

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

}  // namespace

SparkConfig::SparkConfig(std::filesystem::path file) : file_(std::move(file)) {}

bool SparkConfig::load()
{
    last_error_.clear();

    if (!std::filesystem::exists(file_)) {
        return false;
    }

    std::ifstream in(file_);
    if (!in) {
        last_error_ = "Unable to open config file for reading";
        return false;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    toml::parse_result result;
    try {
        result = toml::parse(text);
    }
    catch (const toml::parse_error &) {
        last_error_ = "Malformed TOML in config file - using defaults";
        return false;
    }

    auto viewer_url = result["viewerUrl"].value<std::string>().value_or(this->viewer_url);
    auto bytebin_url = result["bytebinUrl"].value<std::string>().value_or(this->bytebin_url);
    auto bytesocks_host = result["bytesocksHost"].value<std::string>().value_or(this->bytesocks_host);
    auto background_enabled = result["backgroundProfiler"].value<bool>().value_or(background_profiler_enabled);
    auto grouper =
        result["backgroundProfilerThreadGrouper"].value<std::string>().value_or(background_profiler_thread_grouper);
    auto dumper =
        result["backgroundProfilerThreadDumper"].value<std::string>().value_or(background_profiler_thread_dumper);
    auto broadcast = result["disableResponseBroadcast"].value<bool>().value_or(disable_response_broadcast);
    auto interval = result["backgroundProfilerInterval"].value<std::int64_t>().value_or(background_profiler_interval);

    const auto invalid_type = [&result](std::string_view key, const auto &tag) {
        using Value = std::remove_cvref_t<decltype(tag)>;
        return result[key] && !result[key].value<Value>();
    };
    if (invalid_type("viewerUrl", std::string{}) || invalid_type("bytebinUrl", std::string{}) ||
        invalid_type("bytesocksHost", std::string{}) || invalid_type("backgroundProfiler", bool{}) ||
        invalid_type("backgroundProfilerThreadGrouper", std::string{}) ||
        invalid_type("backgroundProfilerThreadDumper", std::string{}) ||
        invalid_type("disableResponseBroadcast", bool{}) ||
        invalid_type("backgroundProfilerInterval", std::int64_t{})) {
        last_error_ = "Invalid type for a spark configuration value - using defaults";
        return false;
    }
    if (viewer_url.empty() || bytebin_url.empty() || bytesocks_host.empty()) {
        last_error_ = "Spark endpoint values must not be empty - using defaults";
        return false;
    }
    if (interval < 1 || interval > KMaxBackgroundProfilerIntervalMs || interval > std::numeric_limits<int>::max()) {
        last_error_ = "backgroundProfilerInterval must be between 1 and 1000 milliseconds - using defaults";
        return false;
    }
    if (grouper != "by-pool" && grouper != "by-name" && grouper != "as-one") {
        last_error_ = "Invalid backgroundProfilerThreadGrouper - using defaults";
        return false;
    }
    if (dumper != "default" && dumper != "all") {
        last_error_ = "Invalid backgroundProfilerThreadDumper - using defaults";
        return false;
    }

    this->viewer_url = std::move(viewer_url);
    this->bytebin_url = std::move(bytebin_url);
    this->bytesocks_host = std::move(bytesocks_host);
    background_profiler_enabled = background_enabled;
    background_profiler_interval = static_cast<int>(interval);
    background_profiler_thread_grouper = std::move(grouper);
    background_profiler_thread_dumper = std::move(dumper);
    disable_response_broadcast = broadcast;

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
    return exists ? load() : save();
}

void SparkConfig::writeTemplate(std::ostream &out) const
{
    out << "# spark configuration file\n";
    out << "# https://spark.lucko.me/docs/Configuration\n";
    out << "\n";
    out << "# URL of the spark viewer\n";
    out << "viewerUrl = \"" << escapeString(viewer_url) << "\"\n";
    out << "\n";
    out << "# URL of the bytebin upload endpoint\n";
    out << "bytebinUrl = \"" << escapeString(bytebin_url) << "\"\n";
    out << "\n";
    out << "# Host of the bytesocks websocket\n";
    out << "bytesocksHost = \"" << escapeString(bytesocks_host) << "\"\n";
    out << "\n";
    out << "# Whether the background profiler should run\n";
    out << "backgroundProfiler = " << (background_profiler_enabled ? "true" : "false") << "\n";
    out << "\n";
    out << "# Background profiler sampling interval in milliseconds\n";
    out << "backgroundProfilerInterval = " << background_profiler_interval << "\n";
    out << "\n";
    out << "# Thread grouping strategy for the background profiler\n";
    out << "backgroundProfilerThreadGrouper = \"" << escapeString(background_profiler_thread_grouper) << "\"\n";
    out << "\n";
    out << "# Thread dumper for the background profiler\n";
    out << "backgroundProfilerThreadDumper = \"" << escapeString(background_profiler_thread_dumper) << "\"\n";
    out << "\n";
    out << "# Disable broadcasting profiler results to all players\n";
    out << "disableResponseBroadcast = " << (disable_response_broadcast ? "true" : "false") << "\n";
}

bool SparkConfig::save() const
{
    last_error_.clear();

    std::ostringstream ss;
    writeTemplate(ss);

    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);

    std::filesystem::path tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) {
            last_error_ = "Unable to open config file for writing";
            return false;
        }
        out << ss.str();
        out.close();
    }
    std::filesystem::rename(tmp, file_, ec);
    if (ec) {
        last_error_ = "Unable to rename config file: " + ec.message();
        return false;
    }
    return true;
}

}  // namespace spark
