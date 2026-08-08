#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/config/spark_config.h"

using namespace spark;

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
    assert(config.load());

    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.background_profiler_enabled == true);

    std::printf("  [PASS] wrong type\n");
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
    std::printf("Running SparkConfig tests...\n");
    test_defaults();
    test_toml_valid_override();
    test_toml_invalid();
    test_toml_wrong_type();
    test_toml_unknown_field();
    test_toml_partial();
    test_save_creates_toml();
    test_save_and_reload();
    test_empty_toml();
    test_toml_with_comments();
    std::printf("All SparkConfig tests passed!\n");
    return 0;
}
