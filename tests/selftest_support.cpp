#include <cstdio>
#include <cstring>
#include <filesystem>
#include <ranges>
#include <stdexcept>

#include "selftest_internal.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>

#include <sys/syscall.h>
#endif

#include "application/health/health_command.h"
#include "core/config/trusted_viewers.h"
#include "core/stats/statistics_service.h"
#include "proto/health_data.h"

namespace spark::selftest {

namespace {

bool readProtoVarintImpl(std::string_view bytes, std::size_t &offset, std::uint64_t &value)
{
    value = 0;
    for (int shift = 0; shift < 64 && offset < bytes.size(); shift += 7) {
        const auto byte = static_cast<unsigned char>(bytes[offset++]);
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

bool nextProtoField(std::string_view bytes, std::size_t &offset, ProtoField &field)
{
    std::uint64_t tag = 0;
    if (!readProtoVarintImpl(bytes, offset, tag) || tag == 0) {
        return false;
    }
    field = ProtoField{};
    field.number = static_cast<int>(tag >> 3);
    field.wire_type = static_cast<int>(tag & 7);
    if (field.wire_type == 0) {
        return readProtoVarintImpl(bytes, offset, field.varint);
    }
    if (field.wire_type == 1) {
        if (offset + sizeof(std::uint64_t) > bytes.size()) {
            return false;
        }
        std::uint64_t bits = 0;
        std::memcpy(&bits, bytes.data() + offset, sizeof(bits));
        std::memcpy(&field.real, &bits, sizeof(bits));
        offset += sizeof(bits);
        return true;
    }
    if (field.wire_type == 2) {
        std::uint64_t size = 0;
        if (!readProtoVarintImpl(bytes, offset, size) || size > bytes.size() - offset) {
            return false;
        }
        field.bytes = bytes.substr(offset, static_cast<std::size_t>(size));
        offset += static_cast<std::size_t>(size);
        return true;
    }
    if (field.wire_type == 5) {
        if (offset + sizeof(std::uint32_t) > bytes.size()) {
            return false;
        }
        offset += sizeof(std::uint32_t);
        return true;
    }
    return false;
}

}  // namespace

bool readProtoVarint(std::string_view bytes, std::size_t &offset, std::uint64_t &value)
{
    return readProtoVarintImpl(bytes, offset, value);
}

bool findProtoField(std::string_view bytes, int number, ProtoField &result, std::size_t occurrence)
{
    std::size_t offset = 0;
    std::size_t matched = 0;
    while (offset < bytes.size()) {
        ProtoField field;
        if (!nextProtoField(bytes, offset, field)) {
            return false;
        }
        if (field.number == number && matched++ == occurrence) {
            result = field;
            return true;
        }
    }
    return false;
}

void TestDispatcher::runOnMainThread(std::function<void()> task)
{
    if (reject_.load()) {
        throw std::runtime_error("dispatcher rejected task");
    }
    task();
}

void TestDispatcher::setReject(bool reject)
{
    reject_.store(reject);
}

void TestMetadataProvider::gatherServerMetadata(spark::ServerMetadata &metadata, std::int64_t /*now_ms*/)
{
    checkThread();
    metadata.server_configurations["server.properties"] = R"({"max-players":"20"})";
}

void TestMetadataProvider::gatherWorldMetadata(spark::WorldInfo & /*world*/, std::string_view /*minecraft_version*/)
{
    checkThread();
}

std::int64_t TestMetadataProvider::serverUptimeSeconds()
{
    return 0;
}

std::int64_t TestMetadataProvider::playerCount()
{
    return 0;
}

spark::PlayerPingProvider *TestMetadataProvider::playerPingProvider()
{
    return nullptr;
}

bool TestMetadataProvider::usedOffThread() const
{
    return used_off_thread_.load();
}

void TestMetadataProvider::checkThread()
{
    if (std::this_thread::get_id() != owner_thread_) {
        used_off_thread_.store(true);
    }
}

void TestNotifier::notify(const std::string & /*sender_name*/, const std::string &text)
{
    std::scoped_lock lock(mutex_);
    messages_.push_back(text);
}

bool TestNotifier::contains(const std::string &text) const
{
    std::scoped_lock lock(mutex_);
    return std::ranges::find(messages_, text) != messages_.end();
}

std::string TestCommandSender::getName() const
{
    return "Console";
}

bool TestCommandSender::isPlayer() const
{
    return false;
}

void TestCommandSender::sendImpl(const std::string &message)
{
    messages.push_back(message);
}

void TestCommandSender::errorImpl(const std::string &message)
{
    errors.push_back(message);
}

void worker(std::atomic<std::uint64_t> &worker_tid, std::atomic<bool> &run)
{
#ifdef _WIN32
    worker_tid.store(static_cast<std::uint64_t>(GetCurrentThreadId()));
#else
    worker_tid.store(static_cast<std::uint64_t>(::syscall(SYS_gettid)));
#endif
    while (run.load()) {
        hotOuter();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

bool verifyHealthServerConfigurations()
{
    spark::StatisticsService statistics;
    TestMetadataProvider metadata_provider;
    TestDispatcher dispatcher;
    TestNotifier notifier;
    TestCommandSender sender;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() / "spark-health-viewers.json");
    spark::HealthCommand health(statistics, metadata_provider, {}, {}, {}, trusted_viewers, dispatcher, notifier);
    const spark::HealthData data = spark::HealthCommandTestAccess::capture(health, sender, 1234);
    const std::string payload = spark::buildHealthData(data);

    ProtoField metadata;
    ProtoField entry;
    ProtoField key;
    ProtoField value;
    if (!findProtoField(payload, 1, metadata) || !findProtoField(metadata.bytes, 6, entry) ||
        !findProtoField(entry.bytes, 1, key) || !findProtoField(entry.bytes, 2, value) ||
        key.bytes != "server.properties" || value.bytes != R"({"max-players":"20"})" ||
        payload.find("level-seed") != std::string::npos ||
        payload.find("server-authoritative-secret") != std::string::npos) {
        std::fprintf(stderr, "health metadata: allowlisted server configuration was not encoded safely\n");
        return false;
    }
    return true;
}

}  // namespace spark::selftest
