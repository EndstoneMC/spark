#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/config/spark_config.h"

using namespace spark;  // NOLINT(google-build-using-namespace)

namespace {

std::filesystem::path tempDir()
{
    auto dir = std::filesystem::temp_directory_path() / "spark_config_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary);
    out << content;
    out.close();
}

void cleanup(const std::filesystem::path &path)
{
    std::filesystem::remove(path);
}

void test_defaults()
{
    auto dir = tempDir();
    auto path = dir / "nonexistent.toml";
    cleanup(path);

    SparkConfig config(path);
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

    std::printf("  [PASS] defaults\n");
}

void test_toml_valid_override()
{
    auto dir = tempDir();
    auto path = dir / "valid_override.toml";
    cleanup(path);

    std::string toml = R"(# spark config
viewerUrl = "https://custom.example.com/"
bytebinUrl = "https://upload.example.com/"
bytesocksHost = "ws.example.com"
backgroundProfiler = false
backgroundProfilerInterval = 20
backgroundProfilerThreadGrouper = "by-name"
backgroundProfilerThreadDumper = "all"
disableResponseBroadcast = true
)";
    writeFile(path, toml);

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

    std::printf("  [PASS] TOML valid override\n");
}

void test_toml_invalid()
{
    auto dir = tempDir();
    auto path = dir / "invalid.toml";
    cleanup(path);

    writeFile(path, "this is = [ not valid toml ");

    SparkConfig config(path);
    bool ok = config.load();
    assert(!ok);
    assert(!config.lastError().empty());

    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.background_profiler_enabled == true);

    std::printf("  [PASS] invalid TOML\n");
}

void test_toml_wrong_type()
{
    auto dir = tempDir();
    auto path = dir / "wrong_type.toml";
    cleanup(path);

    // viewerUrl is a number instead of a string - should be ignored.
    writeFile(path, R"(viewerUrl = 123
backgroundProfiler = "not-a-bool"
)");

    SparkConfig config(path);
    assert(!config.load());

    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.background_profiler_enabled == true);

    std::printf("  [PASS] wrong type\n");
}

void test_bootstrap_preserves_invalid_file()
{
    auto dir = tempDir();
    auto path = dir / "bootstrap_invalid.toml";
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

void test_validation()
{
    auto dir = tempDir();
    auto path = dir / "validation.toml";
    const std::vector<std::string> invalid = {"backgroundProfilerInterval = -1\n",
                                              "backgroundProfilerInterval = 9223372036854775807\n",
                                              "backgroundProfilerThreadGrouper = \"invalid\"\n",
                                              "backgroundProfilerThreadDumper = \"invalid\"\n", "bytebinUrl = \"\"\n"};
    for (const auto &text : invalid) {
        writeFile(path, text);
        SparkConfig config(path);
        assert(!config.load());
        assert(config.background_profiler_interval == 10);
    }
    std::printf("  [PASS] validation\n");
}

void test_endpoint_validation()
{
    auto dir = tempDir();
    auto path = dir / "endpoint_validation.toml";
    const std::vector<std::string> valid = {
        "viewerUrl = \"http://localhost:8080/viewer\"\n",
        "bytebinUrl = \"https://10.0.0.8:8443/content/\"\n",
        "bytesocksHost = \"localhost:9443\"\n",
    };
    for (const auto &text : valid) {
        writeFile(path, text);
        SparkConfig config(path);
        assert(config.load());
    }

    const std::vector<std::string> invalid = {
        "viewerUrl = \"ftp://viewer.example.com/\"\n",
        "viewerUrl = \"https:///missing-host\"\n",
        "viewerUrl = \"https://user@example.com/\"\n",
        "viewerUrl = \"https://viewer.example.com/?token=value\"\n",
        "bytebinUrl = \"https://upload.example.com/\npath\"\n",
        "bytesocksHost = \"relay.example.com/path\"\n",
        "bytesocksHost = \"relay.example.com:invalid\"\n",
        "bytesocksHost = \"relay.example.com 443\"\n",
    };
    for (const auto &text : invalid) {
        writeFile(path, text);
        const std::string &original = text;
        SparkConfig config(path);
        const bool loaded = config.loadOrCreate();
        if (loaded) {
            std::fprintf(stderr, "accepted invalid endpoint: %s", text.c_str());
        }
        assert(!loaded);
        std::ifstream in(path, std::ios::binary);
        const std::string preserved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(preserved == original);
        assert(config.viewer_url == "https://spark.lucko.me/");
    }
    std::printf("  [PASS] endpoint validation\n");
}

void test_toml_unknown_field()
{
    auto dir = tempDir();
    auto path = dir / "unknown_field.toml";
    cleanup(path);

    writeFile(path, R"(unknownField = "hello"
viewerUrl = "https://custom.example.com/"
)");

    SparkConfig config(path);
    assert(config.load());

    assert(config.viewer_url == "https://custom.example.com/");

    std::printf("  [PASS] unknown field\n");
}

void test_toml_partial()
{
    auto dir = tempDir();
    auto path = dir / "partial.toml";
    cleanup(path);

    writeFile(path, R"(viewerUrl = "https://custom.example.com/"
)");

    SparkConfig config(path);
    assert(config.load());

    assert(config.viewer_url == "https://custom.example.com/");
    assert(config.bytebin_url == "https://spark-usercontent.lucko.me/");
    assert(config.background_profiler_enabled == true);

    std::printf("  [PASS] partial config\n");
}

void test_save_creates_toml()
{
    auto dir = tempDir();
    auto path = dir / "save_create.toml";
    cleanup(path);

    SparkConfig config(path);
    assert(config.save());
    assert(std::filesystem::exists(path));

    // The created file should be loadable.
    SparkConfig config2(path);
    assert(config2.load());
    assert(config2.viewer_url == "https://spark.lucko.me/");

    // Verify the file contains comments.
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(content.find("# spark configuration") != std::string::npos);
    assert(content.find("viewerUrl") != std::string::npos);

    std::printf("  [PASS] save creates TOML\n");
}

void test_save_and_reload()
{
    auto dir = tempDir();
    auto path = dir / "save_reload.toml";
    cleanup(path);

    SparkConfig config(path);
    config.save();
    config.load();

    config.viewer_url = "https://saved.example.com/";
    config.bytebin_url = "https://upload-saved.example.com/";
    config.background_profiler_enabled = false;
    config.background_profiler_interval = 25;

    assert(config.save());

    SparkConfig config2(path);
    assert(config2.load());

    assert(config2.viewer_url == "https://saved.example.com/");
    assert(config2.bytebin_url == "https://upload-saved.example.com/");
    assert(config2.background_profiler_enabled == false);
    assert(config2.background_profiler_interval == 25);

    std::printf("  [PASS] save and reload\n");
}

void test_empty_toml()
{
    auto dir = tempDir();
    auto path = dir / "empty.toml";
    cleanup(path);

    writeFile(path, "");

    SparkConfig config(path);
    assert(config.load());

    // All fields should keep defaults.
    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.background_profiler_enabled == true);

    std::printf("  [PASS] empty TOML\n");
}

void test_toml_with_comments()
{
    auto dir = tempDir();
    auto path = dir / "with_comments.toml";
    cleanup(path);

    writeFile(path, R"(# This is a comment
# Another comment
viewerUrl = "https://commented.example.com/"
# Trailing comment
)");

    SparkConfig config(path);
    assert(config.load());
    assert(config.viewer_url == "https://commented.example.com/");

    std::printf("  [PASS] TOML with comments\n");
}

}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Running SparkConfig tests...\n");
    test_defaults();
    test_toml_valid_override();
    test_toml_invalid();
    test_toml_wrong_type();
    test_bootstrap_preserves_invalid_file();
    test_validation();
    test_endpoint_validation();
    test_toml_unknown_field();
    test_toml_partial();
    test_save_creates_toml();
    test_save_and_reload();
    test_empty_toml();
    test_toml_with_comments();
    std::printf("All SparkConfig tests passed!\n");
    return 0;
}
