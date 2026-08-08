#include "core/activity/activity_log.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace spark;

namespace {

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void testSerializeDeserialize()
{
    Activity a = Activity::url("Steve", true, 1234567890, "Profiler",
                               "https://spark.lucko.me/abc123");
    std::string json = a.serialize();
    std::cout << "Serialized: " << json << "\n";

    Activity b;
    assert(Activity::deserialize(json, b));
    assert(b.user_name == "Steve");
    assert(b.user_is_player == true);
    assert(b.time_ms == 1234567890);
    assert(b.type == "Profiler");
    assert(b.data_type == Activity::DataType::Url);
    assert(b.data_value == "https://spark.lucko.me/abc123");

    // Round-trip a file activity.
    Activity c = Activity::file("Console", false, 1234567891, "Profiler",
                                "/plugins/spark/profiles/test.sparkprofile");
    json = c.serialize();
    assert(Activity::deserialize(json, b));
    assert(b.user_name == "Console");
    assert(b.user_is_player == false);
    assert(b.data_type == Activity::DataType::File);
    assert(b.data_value == "/plugins/spark/profiles/test.sparkprofile");

    std::cout << "testSerializeDeserialize: PASS\n";
}

void testSpecialCharacters()
{
    Activity a = Activity::url("Player\"With\"Quotes", false, 100, "Health",
                               "https://example.com/path?x=1&y=2");
    std::string json = a.serialize();
    Activity b;
    assert(Activity::deserialize(json, b));
    assert(b.user_name == "Player\"With\"Quotes");
    assert(b.data_value == "https://example.com/path?x=1&y=2");

    std::cout << "testSpecialCharacters: PASS\n";
}

void testDeserializeInvalid()
{
    Activity a;
    assert(!Activity::deserialize("", a));
    assert(!Activity::deserialize("{}", a));
    assert(!Activity::deserialize("[]", a));
    assert(!Activity::deserialize("{\"user\":{}}", a));
    assert(!Activity::deserialize("not json at all", a));

    std::cout << "testDeserializeInvalid: PASS\n";
}

void testExpiry()
{
    std::int64_t now = nowMs();
    // URL activity: expires after 60 days.
    Activity url_old = Activity::url("P", false, now - 61 * 24 * 3600 * 1000LL,
                                     "Profiler", "https://example.com/old");
    assert(url_old.shouldExpire(now));

    Activity url_recent = Activity::url("P", false, now - 30 * 24 * 3600 * 1000LL,
                                        "Profiler", "https://example.com/recent");
    assert(!url_recent.shouldExpire(now));

    // File activity: never expires.
    Activity file_old = Activity::file("P", false, now - 365 * 24 * 3600 * 1000LL,
                                       "Profiler", "/path/to/file");
    assert(!file_old.shouldExpire(now));

    std::cout << "testExpiry: PASS\n";
}

void testLogLoadSave()
{
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "spark_activity_test.json";
    std::filesystem::remove(tmp);

    // Empty log loads fine from a non-existent file.
    ActivityLog log(tmp);
    log.load();
    assert(log.entries().empty());

    // Add activities.
    log.add(Activity::url("Alice", true, 100, "Profiler", "https://spark.lucko.me/a1"));
    log.add(Activity::file("Bob", false, 200, "Profiler", "/profiles/p1.sparkprofile"));
    log.add(Activity::url("Console", false, 300, "Health report", "https://spark.lucko.me/h1"));

    assert(log.entries().size() == 3);
    // Newest first.
    assert(log.entries()[0].type == "Health report");
    assert(log.entries()[1].type == "Profiler");
    assert(log.entries()[2].user_name == "Alice");

    // Reload from disk.
    ActivityLog log2(tmp);
    log2.load();
    assert(log2.entries().size() == 3);
    assert(log2.entries()[0].type == "Health report");
    assert(log2.entries()[1].data_type == Activity::DataType::File);
    assert(log2.entries()[2].user_name == "Alice");

    std::filesystem::remove(tmp);
    std::cout << "testLogLoadSave: PASS\n";
}

void testLogCorruptionSafe()
{
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "spark_activity_corrupt.json";

    // Write a partially corrupted JSON array.
    {
        std::ofstream out(tmp);
        out << R"([
            {"user":{"name":"Good","isPlayer":false},"time":100,"type":"Profiler","data":{"type":"url","value":"https://example.com/good"}},
            {"corrupt":"entry"},
            {"user":{"name":"AlsoGood","isPlayer":true},"time":200,"type":"Health report","data":{"type":"url","value":"https://example.com/good2"}}
        ])";
    }

    ActivityLog log(tmp);
    log.load();
    assert(log.entries().size() == 2);
    assert(log.entries()[0].user_name == "AlsoGood");
    assert(log.entries()[1].user_name == "Good");

    std::filesystem::remove(tmp);
    std::cout << "testLogCorruptionSafe: PASS\n";
}

void testMaxEntries()
{
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "spark_activity_max.json";
    std::filesystem::remove(tmp);

    ActivityLog log(tmp);
    for (int i = 0; i < 120; ++i) {
        log.add(Activity::url("U", false, i, "T", "https://example.com/" + std::to_string(i)));
    }
    assert(log.entries().size() == ActivityLog::kMaxEntries);
    // Newest first: entry 119 should be at index 0.
    assert(log.entries()[0].time_ms == 119);

    std::filesystem::remove(tmp);
    std::cout << "testMaxEntries: PASS\n";
}

}  // namespace

int main()
{
    testSerializeDeserialize();
    testSpecialCharacters();
    testDeserializeInvalid();
    testExpiry();
    testLogLoadSave();
    testLogCorruptionSafe();
    testMaxEntries();
    std::cout << "All activity log tests passed.\n";
    return 0;
}
