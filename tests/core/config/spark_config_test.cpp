#include "core/config/spark_config.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace spark;

namespace {

// Helper: write content to a temp file and return its path.
std::filesystem::path writeFile(const std::string &name, const std::string &content)
{
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary);
    out << content;
    out.close();
    return path;
}

void test_defaults()
{
    auto path = std::filesystem::temp_directory_path() / "spark_config_nonexistent.json";
    std::filesystem::remove(path);

    SparkConfig config(path);
    // load() returns false for missing file, but fields keep defaults.
    bool ok = config.load();
    assert(!ok);

    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.bytebin_url == "https://spark-usercontent.lucko.me/");
    assert(config.bytesocks_host == "spark-usersockets.lucko.me");
    assert(config.background_profiler_enabled == true);
    assert(config.background_profiler_interval == 10);
    assert(config.background_profiler_thread_grouper == "by-pool");
    assert(config.background_profiler_thread_dumper == "default");
    assert(config.disable_response_broadcast == false);
    assert(config.trusted_keys.empty());

    std::printf("  [PASS] defaults\n");
}

void test_valid_override()
{
    std::string json = R"({
        "viewerUrl": "https://custom.example.com/",
        "bytebinUrl": "https://upload.example.com/",
        "bytesocksHost": "ws.example.com",
        "backgroundProfiler": false,
        "backgroundProfilerInterval": 20,
        "backgroundProfilerThreadGrouper": "by-name",
        "backgroundProfilerThreadDumper": "all",
        "disableResponseBroadcast": true,
        "trustedKeys": ["key1", "key2"]
    })";
    auto path = writeFile("spark_config_valid.json", json);

    SparkConfig config(path);
    assert(config.load());

    assert(config.viewer_url == "https://custom.example.com/");
    assert(config.bytebin_url == "https://upload.example.com/");
    assert(config.bytesocks_host == "ws.example.com");
    assert(config.background_profiler_enabled == false);
    assert(config.background_profiler_interval == 20);
    assert(config.background_profiler_thread_grouper == "by-name");
    assert(config.background_profiler_thread_dumper == "all");
    assert(config.disable_response_broadcast == true);
    assert(config.trusted_keys.size() == 2);
    assert(config.trusted_keys[0] == "key1");
    assert(config.trusted_keys[1] == "key2");

    std::printf("  [PASS] valid override\n");
}

void test_invalid_json()
{
    auto path = writeFile("spark_config_invalid.json", "{ this is not valid json }");

    SparkConfig config(path);
    bool ok = config.load();
    assert(!ok);
    assert(!config.lastError().empty());

    // Fields should keep defaults.
    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.background_profiler_enabled == true);

    std::printf("  [PASS] invalid JSON\n");
}

void test_wrong_type()
{
    // viewerUrl is a number instead of a string - should be ignored.
    std::string json = R"({"viewerUrl": 123, "backgroundProfiler": "not-a-bool"})";
    auto path = writeFile("spark_config_wrong_type.json", json);

    SparkConfig config(path);
    assert(config.load());

    // Defaults should be preserved for wrong-type fields.
    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.background_profiler_enabled == true);

    std::printf("  [PASS] wrong type\n");
}

void test_unknown_field()
{
    std::string json = R"({"unknownField": "hello", "viewerUrl": "https://custom.example.com/"})";
    auto path = writeFile("spark_config_unknown.json", json);

    SparkConfig config(path);
    assert(config.load());

    // Unknown field ignored, known field loaded.
    assert(config.viewer_url == "https://custom.example.com/");

    std::printf("  [PASS] unknown field\n");
}

void test_partial_config()
{
    // Only some fields present; others should keep defaults.
    std::string json = R"({"viewerUrl": "https://custom.example.com/"})";
    auto path = writeFile("spark_config_partial.json", json);

    SparkConfig config(path);
    assert(config.load());

    assert(config.viewer_url == "https://custom.example.com/");
    assert(config.bytebin_url == "https://spark-usercontent.lucko.me/");
    assert(config.background_profiler_enabled == true);

    std::printf("  [PASS] partial config\n");
}

void test_save_and_reload()
{
    auto path = writeFile("spark_config_save.json", "{}");

    SparkConfig config(path);
    config.load();
    config.viewer_url = "https://saved.example.com/";
    config.bytebin_url = "https://upload-saved.example.com/";
    config.background_profiler_enabled = false;
    config.background_profiler_interval = 25;
    config.trusted_keys = {"abc", "def"};

    assert(config.save());

    // Reload in a new instance.
    SparkConfig config2(path);
    assert(config2.load());

    assert(config2.viewer_url == "https://saved.example.com/");
    assert(config2.bytebin_url == "https://upload-saved.example.com/");
    assert(config2.background_profiler_enabled == false);
    assert(config2.background_profiler_interval == 25);
    assert(config2.trusted_keys.size() == 2);
    assert(config2.trusted_keys[0] == "abc");
    assert(config2.trusted_keys[1] == "def");

    std::printf("  [PASS] save and reload\n");
}

void test_save_creates_file()
{
    auto path = std::filesystem::temp_directory_path() / "spark_config_create.json";
    std::filesystem::remove(path);

    SparkConfig config(path);
    // No file exists yet - save should create it.
    assert(config.save());
    assert(std::filesystem::exists(path));

    // The created file should be loadable.
    SparkConfig config2(path);
    assert(config2.load());
    assert(config2.viewer_url == "https://spark.lucko.me/");

    std::printf("  [PASS] save creates file\n");
}

void test_empty_trusted_keys()
{
    std::string json = R"({"trustedKeys": []})";
    auto path = writeFile("spark_config_empty_keys.json", json);

    SparkConfig config(path);
    assert(config.load());
    assert(config.trusted_keys.empty());

    std::printf("  [PASS] empty trusted keys\n");
}

void test_trusted_keys_with_non_string_entries()
{
    std::string json = R"({"trustedKeys": ["key1", 123, true, "key2"]})";
    auto path = writeFile("spark_config_mixed_keys.json", json);

    SparkConfig config(path);
    assert(config.load());
    assert(config.trusted_keys.size() == 2);
    assert(config.trusted_keys[0] == "key1");
    assert(config.trusted_keys[1] == "key2");

    std::printf("  [PASS] trusted keys with non-string entries\n");
}

}  // namespace

int main()
{
    std::printf("Running SparkConfig tests...\n");
    test_defaults();
    test_valid_override();
    test_invalid_json();
    test_wrong_type();
    test_unknown_field();
    test_partial_config();
    test_save_and_reload();
    test_save_creates_file();
    test_empty_trusted_keys();
    test_trusted_keys_with_non_string_entries();
    std::printf("All SparkConfig tests passed!\n");
    return 0;
}
