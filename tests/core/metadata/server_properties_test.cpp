#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/metadata/server_properties.h"

using namespace spark;

static std::filesystem::path writeTempFile(const std::string &name, const std::string &content)
{
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary);
    out << content;
    out.close();
    return path;
}

int main()
{
    // Missing file -> empty map
    {
        auto result = parseServerProperties("nonexistent_server.properties");
        assert(result.empty());
    }

    // Empty file -> empty map
    {
        auto path = writeTempFile("empty.properties", "");
        auto result = parseServerProperties(path);
        assert(result.empty());
        std::filesystem::remove(path);
    }

    // Comments and blank lines ignored
    {
        auto path = writeTempFile("comments.properties", "# This is a comment\n"
                                                         "\n"
                                                         "# max-players=999\n"
                                                         "max-players=20\n"
                                                         "\n");
        auto result = parseServerProperties(path);
        assert(result.size() == 1);
        assert(result.at("max-players") == "20");
        std::filesystem::remove(path);
    }

    // Allowlisted keys returned
    {
        auto path = writeTempFile("allowlisted.properties", "max-players=50\n"
                                                            "view-distance=8\n"
                                                            "tick-distance=4\n"
                                                            "max-threads=8\n"
                                                            "compression-threshold=256\n"
                                                            "compression-algorithm=1\n");
        auto result = parseServerProperties(path);
        assert(result.size() == 6);
        assert(result.at("max-players") == "50");
        assert(result.at("view-distance") == "8");
        assert(result.at("tick-distance") == "4");
        assert(result.at("max-threads") == "8");
        assert(result.at("compression-threshold") == "256");
        assert(result.at("compression-algorithm") == "1");
        std::filesystem::remove(path);
    }

    // level-seed excluded
    {
        auto path = writeTempFile("seed.properties", "level-seed=mysecretseed\n"
                                                     "max-players=20\n");
        auto result = parseServerProperties(path);
        assert(result.size() == 1);
        assert(result.count("level-seed") == 0);
        assert(result.at("max-players") == "20");
        std::filesystem::remove(path);
    }

    // server-port excluded
    {
        auto path = writeTempFile("port.properties", "server-port=19132\n"
                                                     "server-portv6=19133\n"
                                                     "max-players=20\n");
        auto result = parseServerProperties(path);
        assert(result.size() == 1);
        assert(result.count("server-port") == 0);
        assert(result.count("server-portv6") == 0);
        std::filesystem::remove(path);
    }

    // script-debugger-passcode excluded
    {
        auto path = writeTempFile("passcode.properties", "script-debugger-passcode=secretpass\n"
                                                         "script-debugger-auto-attach-connect-address=127.0.0.1:1234\n"
                                                         "max-players=20\n");
        auto result = parseServerProperties(path);
        assert(result.size() == 1);
        assert(result.count("script-debugger-passcode") == 0);
        assert(result.count("script-debugger-auto-attach-connect-address") == 0);
        std::filesystem::remove(path);
    }

    // Commented watchdog setting excluded
    {
        auto path = writeTempFile("commented_watchdog.properties", "# script-watchdog-enable=true\n"
                                                                   "script-watchdog-enable=true\n"
                                                                   "max-players=20\n");
        auto result = parseServerProperties(path);
        assert(result.size() == 2);
        assert(result.at("script-watchdog-enable") == "true");
        assert(result.at("max-players") == "20");
        std::filesystem::remove(path);
    }

    // Value containing '='
    {
        auto path = writeTempFile("equals.properties", "content-log-level=info=verbose\n"
                                                       "max-players=20\n");
        auto result = parseServerProperties(path);
        assert(result.size() == 2);
        assert(result.at("content-log-level") == "info=verbose");
        assert(result.at("max-players") == "20");
        std::filesystem::remove(path);
    }

    // CRLF line endings
    {
        auto path = writeTempFile("crlf.properties", "max-players=20\r\n"
                                                     "view-distance=8\r\n");
        auto result = parseServerProperties(path);
        assert(result.size() == 2);
        assert(result.at("max-players") == "20");
        assert(result.at("view-distance") == "8");
        std::filesystem::remove(path);
    }

    // Unknown keys ignored
    {
        auto path = writeTempFile("unknown.properties", "unknown-key=value\n"
                                                        "server-name=My Server\n"
                                                        "gamemode=survival\n"
                                                        "max-players=20\n");
        auto result = parseServerProperties(path);
        assert(result.size() == 1);
        assert(result.count("unknown-key") == 0);
        assert(result.count("server-name") == 0);
        assert(result.count("gamemode") == 0);
        assert(result.at("max-players") == "20");
        std::filesystem::remove(path);
    }

    // Malformed line ignored
    {
        auto path = writeTempFile("malformed.properties", "this-is-not-valid\n"
                                                          "=nokey\n"
                                                          "max-players=20\n"
                                                          "   \n");
        auto result = parseServerProperties(path);
        assert(result.size() == 1);
        assert(result.at("max-players") == "20");
        std::filesystem::remove(path);
    }

    // Extended allowlist keys
    {
        auto path = writeTempFile("extended.properties", "content-log-file-enabled=true\n"
                                                         "content-log-console-output-enabled=false\n"
                                                         "content-log-level=warning\n"
                                                         "client-side-chunk-generation-enabled=true\n"
                                                         "server-build-radius-ratio=0.5\n"
                                                         "server-authoritative-movement-strict=true\n"
                                                         "server-authoritative-dismount-strict=false\n"
                                                         "server-authoritative-entity-interactions-strict=true\n"
                                                         "script-watchdog-enable=true\n"
                                                         "script-watchdog-hang-threshold=3000\n"
                                                         "script-watchdog-spike-threshold=100\n"
                                                         "script-watchdog-slow-threshold=50\n"
                                                         "max-players=20\n");
        auto result = parseServerProperties(path);
        assert(result.size() == 13);
        assert(result.at("script-watchdog-hang-threshold") == "3000");
        assert(result.at("server-build-radius-ratio") == "0.5");
        std::filesystem::remove(path);
    }

    // Whitespace around key and value trimmed
    {
        auto path = writeTempFile("whitespace.properties", "  max-players  =  20  \n"
                                                           "  view-distance = 8 \n");
        auto result = parseServerProperties(path);
        assert(result.size() == 2);
        assert(result.at("max-players") == "20");
        assert(result.at("view-distance") == "8");
        std::filesystem::remove(path);
    }

    // --- serverPropertiesToJsonString tests ---

    // Empty map -> "{}"
    {
        std::map<std::string, std::string> empty;
        assert(serverPropertiesToJsonString(empty) == "{}");
    }

    // Boolean values become JSON booleans
    {
        std::map<std::string, std::string> props = {{"flag", "true"}, {"other", "false"}};
        std::string json = serverPropertiesToJsonString(props);
        assert(json == "{\"flag\":true,\"other\":false}");
    }

    // Numeric values become JSON numbers
    {
        std::map<std::string, std::string> props = {{"max-players", "20"}, {"port", "19132"}};
        std::string json = serverPropertiesToJsonString(props);
        assert(json == "{\"max-players\":20,\"port\":19132}");
    }

    // String values become JSON strings
    {
        std::map<std::string, std::string> props = {{"level", "info"}, {"algo", "snappy"}};
        std::string json = serverPropertiesToJsonString(props);
        assert(json == "{\"algo\":\"snappy\",\"level\":\"info\"}");
    }

    // Mixed types
    {
        std::map<std::string, std::string> props = {
            {"max-players", "20"},
            {"client-side-chunk-generation-enabled", "true"},
            {"compression-algorithm", "snappy"},
            {"server-build-radius-ratio", "Disabled"},
        };
        std::string json = serverPropertiesToJsonString(props);
        assert(json == "{\"client-side-chunk-generation-enabled\":true,"
                       "\"compression-algorithm\":\"snappy\","
                       "\"max-players\":20,"
                       "\"server-build-radius-ratio\":\"Disabled\"}");
    }

    // String values with special characters are escaped
    {
        std::map<std::string, std::string> props = {{"key", "a\"b\\c"}};
        std::string json = serverPropertiesToJsonString(props);
        assert(json == "{\"key\":\"a\\\"b\\\\c\"}");
    }

    std::printf("All server.properties parser tests passed.\n");
    return 0;
}
