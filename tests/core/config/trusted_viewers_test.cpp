#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/config/trusted_viewers.h"

using namespace spark;  // NOLINT(google-build-using-namespace)

namespace {

std::filesystem::path tempDir()
{
    auto dir = std::filesystem::temp_directory_path() / "spark_trusted_viewers_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary);
    out << content;
    out.close();
}

void test_load_missing_file()
{
    auto dir = tempDir();
    auto path = dir / "missing.json";
    std::filesystem::remove(path);

    TrustedViewersState tv(path);
    assert(!tv.load());
    assert(tv.keys().empty());

    std::printf("  [PASS] load missing file\n");
}

void test_load_valid()
{
    auto dir = tempDir();
    auto path = dir / "valid.json";
    writeFile(path, R"(["key1", "key2", "key3"])");

    TrustedViewersState tv(path);
    assert(tv.load());
    assert(tv.keys().size() == 3);
    assert(tv.keys()[0] == "key1");
    assert(tv.keys()[1] == "key2");
    assert(tv.keys()[2] == "key3");

    std::printf("  [PASS] load valid\n");
}

void test_load_empty_array()
{
    auto dir = tempDir();
    auto path = dir / "empty_array.json";
    writeFile(path, "[]");

    TrustedViewersState tv(path);
    assert(tv.load());
    assert(tv.keys().empty());

    std::printf("  [PASS] load empty array\n");
}

void test_load_pretty_printed()
{
    auto dir = tempDir();
    auto path = dir / "pretty.json";
    writeFile(path, "[\n  \"alpha\",\n  \"beta\"\n]\n");

    TrustedViewersState tv(path);
    assert(tv.load());
    assert(tv.keys().size() == 2);
    assert(tv.keys()[0] == "alpha");
    assert(tv.keys()[1] == "beta");

    std::printf("  [PASS] load pretty-printed\n");
}

void test_load_malformed()
{
    auto dir = tempDir();
    auto path = dir / "malformed.json";
    writeFile(path, "{ this is not valid }");

    TrustedViewersState tv(path);
    assert(!tv.load());
    assert(!tv.lastError().empty());
    assert(tv.keys().empty());

    std::printf("  [PASS] load malformed\n");
}

void test_contains()
{
    auto dir = tempDir();
    auto path = dir / "contains.json";
    writeFile(path, R"(["abc", "def"])");

    TrustedViewersState tv(path);
    tv.load();

    assert(tv.contains("abc"));
    assert(tv.contains("def"));
    assert(!tv.contains("xyz"));

    std::printf("  [PASS] contains\n");
}

void test_add_and_save()
{
    auto dir = tempDir();
    auto path = dir / "add_save.json";
    std::filesystem::remove(path);

    TrustedViewersState tv(path);
    tv.add("key1");
    tv.add("key2");
    assert(tv.save());
    assert(std::filesystem::exists(path));

    // Reload and verify.
    TrustedViewersState tv2(path);
    assert(tv2.load());
    assert(tv2.keys().size() == 2);
    assert(tv2.keys()[0] == "key1");
    assert(tv2.keys()[1] == "key2");

    std::printf("  [PASS] add and save\n");
}

void test_add_duplicate()
{
    auto dir = tempDir();
    auto path = dir / "duplicate.json";
    std::filesystem::remove(path);

    TrustedViewersState tv(path);
    tv.add("key1");
    tv.add("key1");  // Duplicate should be ignored.
    assert(tv.keys().size() == 1);

    std::printf("  [PASS] add duplicate ignored\n");
}

void test_save_pretty_format()
{
    auto dir = tempDir();
    auto path = dir / "pretty_format.json";
    std::filesystem::remove(path);

    TrustedViewersState tv(path);
    tv.add("alpha");
    tv.add("beta");
    tv.save();

    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Should be multi-line with indentation.
    assert(content.find("[\n") != std::string::npos);
    assert(content.find("  \"alpha\"") != std::string::npos);
    assert(content.find("  \"beta\"") != std::string::npos);
    assert(content.back() == '\n');

    std::printf("  [PASS] save pretty format\n");
}

}  // namespace

int main()
{
    std::printf("Running TrustedViewersState tests...\n");
    test_load_missing_file();
    test_load_valid();
    test_load_empty_array();
    test_load_pretty_printed();
    test_load_malformed();
    test_contains();
    test_add_and_save();
    test_add_duplicate();
    test_save_pretty_format();
    std::printf("All TrustedViewersState tests passed!\n");
    return 0;
}
