#include "core/metadata/server_properties.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace spark {

namespace {

// Strict allowlist of server.properties keys that are safe to include
// in profile metadata. Any key not in this list is silently dropped.
constexpr std::array<std::string_view, 18> KAllowlist = {
    // Core performance diagnostics
    "max-players",
    "view-distance",
    "tick-distance",
    "max-threads",
    "compression-threshold",
    "compression-algorithm",
    // Logging diagnostics
    "content-log-file-enabled",
    "content-log-console-output-enabled",
    "content-log-level",
    // Generation/performance
    "client-side-chunk-generation-enabled",
    "server-build-radius-ratio",
    // Authoritative movement
    "server-authoritative-movement-strict",
    "server-authoritative-dismount-strict",
    "server-authoritative-entity-interactions-strict",
    // Script watchdog
    "script-watchdog-enable",
    "script-watchdog-hang-threshold",
    "script-watchdog-spike-threshold",
    "script-watchdog-slow-threshold",
};

bool isAllowlisted(std::string_view key)
{
    return std::ranges::any_of(KAllowlist, [key](std::string_view candidate) { return key == candidate; });
}

std::string trim(std::string_view s)
{
    std::size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return std::string(s.substr(start, end - start));
}

bool isAllDigits(std::string_view s)
{
    if (s.empty()) {
        return false;
    }
    return std::ranges::all_of(s, [](char value) { return value >= '0' && value <= '9'; });
}

void appendJsonString(std::string &out, std::string_view value)
{
    out += '"';
    for (char c : value) {
        switch (c) {
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
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else {
                out += c;
            }
            break;
        }
    }
    out += '"';
}

}  // namespace

std::map<std::string, std::string> parseServerProperties(const std::filesystem::path &file)
{
    std::map<std::string, std::string> result;
    if (!std::filesystem::exists(file)) {
        return result;
    }

    std::ifstream in(file);
    if (!in) {
        return result;
    }

    std::string line;
    while (std::getline(in, line)) {
        // Trim trailing CR/LF (handled by trim, but clear the line for safety)
        std::string trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        // Skip comment lines
        if (trimmed[0] == '#') {
            continue;
        }
        // Split on first '='
        std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));
        if (key.empty()) {
            continue;
        }
        // Only allowlisted keys are returned
        if (!isAllowlisted(key)) {
            continue;
        }
        result[key] = value;
    }

    return result;
}

std::string serverPropertiesToJsonString(const std::map<std::string, std::string> &properties)
{
    std::string out;
    out += '{';
    bool first = true;
    for (const auto &[key, value] : properties) {
        if (!first) {
            out += ',';
        }
        first = false;
        appendJsonString(out, key);
        out += ':';
        if (value == "true" || value == "false" || isAllDigits(value)) {
            out += value;
        }
        else {
            appendJsonString(out, value);
        }
    }
    out += '}';
    return out;
}

}  // namespace spark
