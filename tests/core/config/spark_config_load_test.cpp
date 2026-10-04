#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "core/config/spark_config.h"
#include "spark_config_test_cases.h"
#include "spark_config_test_support.h"

namespace spark::config_test {

void testDefaults()
{
    const auto path = tempDir() / "nonexistent.toml";
    cleanup(path);
    SparkConfig config(path);
    assert(!config.load());
    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.bytebin_url == "https://spark-usercontent.lucko.me/");
    assert(config.bytesocks_host == "spark-usersockets.lucko.me");
    assert(config.background_profiler_enabled);
    assert(config.background_profiler_interval == 10);
    assert(config.background_profiler_thread_grouper == "by-pool");
    assert(config.background_profiler_thread_dumper == "default");
    assert(!config.auto_profiler_enabled);
    assert(config.auto_profiler_mspt_threshold == 50.0);
    assert(config.auto_profiler_trigger_duration_seconds == 5);
    assert(config.auto_profiler_duration_seconds == 60);
    assert(config.auto_profiler_interval == 4);
    assert(config.auto_profiler_cooldown_seconds == 300);
    assert(config.auto_profiler_thread_grouper == "by-pool");
    assert(config.auto_profiler_thread_dumper == "default");
    assert(config.allocation_rate_metrics_enabled);
    assert(config.server_properties_additional_keys.empty());
    assert(!config.disable_response_broadcast);
    std::printf("  [PASS] defaults\n");
}

void testTomlValidOverride()
{
    const auto path = tempDir() / "valid_override.toml";
    cleanup(path);
    writeFile(path, R"(# spark config
viewerUrl = "https://custom.example.com/"
bytebinUrl = "https://upload.example.com/"
bytesocksHost = "ws.example.com"
backgroundProfiler = false
backgroundProfilerInterval = 20
backgroundProfilerThreadGrouper = "by-name"
backgroundProfilerThreadDumper = "all"
autoProfiler = true
autoProfilerMsptThreshold = 55.5
autoProfilerTriggerDuration = 7
autoProfilerDuration = 120
autoProfilerInterval = 2
autoProfilerCooldown = 600
autoProfilerThreadGrouper = "as-one"
autoProfilerThreadDumper = "all"
allocationRateMetrics = true
serverPropertiesAdditionalKeys = "server-port, custom-safe-key,server-port"
disableResponseBroadcast = true
)");
    SparkConfig config(path);
    assert(config.load());
    assert(config.viewer_url == "https://custom.example.com/");
    assert(config.bytebin_url == "https://upload.example.com/");
    assert(config.bytesocks_host == "ws.example.com");
    assert(!config.background_profiler_enabled);
    assert(config.background_profiler_interval == 20);
    assert(config.background_profiler_thread_grouper == "by-name");
    assert(config.background_profiler_thread_dumper == "all");
    assert(config.auto_profiler_enabled);
    assert(config.auto_profiler_mspt_threshold == 55.5);
    assert(config.auto_profiler_trigger_duration_seconds == 7);
    assert(config.auto_profiler_duration_seconds == 120);
    assert(config.auto_profiler_interval == 2);
    assert(config.auto_profiler_cooldown_seconds == 600);
    assert(config.auto_profiler_thread_grouper == "as-one");
    assert(config.auto_profiler_thread_dumper == "all");
    assert(config.allocation_rate_metrics_enabled);
    assert(config.server_properties_additional_keys.size() == 2);
    assert(config.server_properties_additional_keys[0] == "server-port");
    assert(config.server_properties_additional_keys[1] == "custom-safe-key");
    assert(config.disable_response_broadcast);
    std::printf("  [PASS] TOML valid override\n");
}

void testTomlInvalid()
{
    const auto path = tempDir() / "invalid.toml";
    cleanup(path);
    writeFile(path, "this is = [ not valid toml ");
    SparkConfig config(path);
    assert(!config.load());
    assert(!config.lastError().empty());
    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.background_profiler_enabled);
    std::printf("  [PASS] invalid TOML\n");
}

void testTomlWrongType()
{
    const auto path = tempDir() / "wrong_type.toml";
    cleanup(path);
    writeFile(path, R"(viewerUrl = 123
backgroundProfiler = "not-a-bool"
allocationRateMetrics = "not-a-bool"
serverPropertiesAdditionalKeys = ["server-port"]
)");
    SparkConfig config(path);
    assert(!config.load());
    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.background_profiler_enabled);
    assert(config.server_properties_additional_keys.empty());
    std::printf("  [PASS] wrong type\n");
}

void testBootstrapPreservesInvalidFile()
{
    const auto path = tempDir() / "bootstrap_invalid.toml";
    cleanup(path);
    const std::string malformed = "viewerUrl = \"unterminated";
    writeFile(path, malformed);
    SparkConfig config(path);
    assert(!config.loadOrCreate());
    std::ifstream in(path, std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(actual == malformed);
    in.close();
    cleanup(path);
    SparkConfig missing(path);
    assert(missing.loadOrCreate());
    assert(std::filesystem::exists(path));
    std::printf("  [PASS] bootstrap preserves invalid file\n");
}

void testValidation()
{
    const auto path = tempDir() / "validation.toml";
    const std::vector<std::string> invalid = {"backgroundProfilerInterval = -1\n",
                                              "backgroundProfilerInterval = 9223372036854775807\n",
                                              "backgroundProfilerThreadGrouper = \"invalid\"\n",
                                              "backgroundProfilerThreadDumper = \"invalid\"\n",
                                              "autoProfilerMsptThreshold = 0.0\n",
                                              "autoProfilerTriggerDuration = 0\n",
                                              "autoProfilerDuration = 601\n",
                                              "autoProfilerInterval = 0\n",
                                              "autoProfilerCooldown = -1\n",
                                              "autoProfilerThreadGrouper = \"invalid\"\n",
                                              "autoProfilerThreadDumper = \"invalid\"\n",
                                              "serverPropertiesAdditionalKeys = \"valid-key,bad key\"\n",
                                              "bytebinUrl = \"\"\n"};
    for (const auto &text : invalid) {
        writeFile(path, text);
        SparkConfig config(path);
        assert(!config.load());
        assert(config.background_profiler_interval == 10);
        assert(config.server_properties_additional_keys.empty());
    }
    std::printf("  [PASS] validation\n");
}

void testEndpointValidation()
{
    const auto path = tempDir() / "endpoint_validation.toml";
    const std::vector<std::string> valid = {"viewerUrl = \"http://localhost:8080/viewer\"\n",
                                            "bytebinUrl = \"https://10.0.0.8:8443/content/\"\n",
                                            "bytesocksHost = \"localhost:9443\"\n"};
    for (const auto &text : valid) {
        writeFile(path, text);
        SparkConfig config(path);
        assert(config.load());
    }
    const std::vector<std::string> invalid = {"viewerUrl = \"ftp://viewer.example.com/\"\n",
                                              "viewerUrl = \"https:///missing-host\"\n",
                                              "viewerUrl = \"https://user@example.com/\"\n",
                                              "viewerUrl = \"https://viewer.example.com/?token=value\"\n",
                                              "bytebinUrl = \"https://upload.example.com/\npath\"\n",
                                              "bytesocksHost = \"relay.example.com/path\"\n",
                                              "bytesocksHost = \"relay.example.com:invalid\"\n",
                                              "bytesocksHost = \"relay.example.com 443\"\n"};
    for (const auto &text : invalid) {
        writeFile(path, text);
        SparkConfig config(path);
        const bool loaded = config.loadOrCreate();
        if (loaded) {
            std::fprintf(stderr, "accepted invalid endpoint: %s", text.c_str());
        }
        assert(!loaded);
        std::ifstream in(path, std::ios::binary);
        const std::string preserved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(preserved == text);
        assert(config.viewer_url == "https://spark.lucko.me/");
    }
    std::printf("  [PASS] endpoint validation\n");
}

void testTomlUnknownField()
{
    const auto path = tempDir() / "unknown_field.toml";
    cleanup(path);
    writeFile(path, R"(unknownField = "hello"
viewerUrl = "https://custom.example.com/"
)");
    SparkConfig config(path);
    assert(config.load());
    assert(config.viewer_url == "https://custom.example.com/");
    std::printf("  [PASS] unknown field\n");
}

void testTomlPartial()
{
    const auto path = tempDir() / "partial.toml";
    cleanup(path);
    writeFile(path, "viewerUrl = \"https://custom.example.com/\"\n");
    SparkConfig config(path);
    assert(config.load());
    assert(config.viewer_url == "https://custom.example.com/");
    assert(config.bytebin_url == "https://spark-usercontent.lucko.me/");
    assert(config.background_profiler_enabled);
    assert(!config.auto_profiler_enabled);
    assert(config.auto_profiler_duration_seconds == 60);
    assert(config.allocation_rate_metrics_enabled);
    assert(config.server_properties_additional_keys.empty());
    std::printf("  [PASS] partial config\n");
}

}  // namespace spark::config_test
