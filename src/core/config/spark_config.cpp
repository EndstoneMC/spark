#include "core/config/spark_config.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <toml.hpp>

namespace spark {

namespace {

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

    // Strings
    if (auto v = result["viewerUrl"].value<std::string>()) {
        viewer_url = *v;
    }
    if (auto v = result["bytebinUrl"].value<std::string>()) {
        bytebin_url = *v;
    }
    if (auto v = result["bytesocksHost"].value<std::string>()) {
        bytesocks_host = *v;
    }
    if (auto v = result["backgroundProfilerThreadGrouper"].value<std::string>()) {
        background_profiler_thread_grouper = *v;
    }
    if (auto v = result["backgroundProfilerThreadDumper"].value<std::string>()) {
        background_profiler_thread_dumper = *v;
    }

    // Booleans
    if (auto v = result["backgroundProfiler"].value<bool>()) {
        background_profiler_enabled = *v;
    }
    if (auto v = result["disableResponseBroadcast"].value<bool>()) {
        disable_response_broadcast = *v;
    }

    // Integers
    if (auto v = result["backgroundProfilerInterval"].value<int64_t>()) {
        background_profiler_interval = static_cast<int>(*v);
    }

    return true;
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
    out << "# Interval (in seconds) between background profiles\n";
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
