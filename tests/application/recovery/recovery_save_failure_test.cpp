#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "application/spark_application.h"
#include "core/recovery/recovery_writer.h"

namespace {

class TestDispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override { task(); }
};

class TestMetadataProvider final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ExportContext & /*ctx*/, std::int64_t /*now_ms*/) override {}
    void gatherWorldMetadata(spark::ExportContext & /*ctx*/) override {}
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }
};

class TestNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string & /*sender_name*/, const std::string &text) override { messages.push_back(text); }
    std::vector<std::string> messages;
};

}  // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_recovery_save_failure_test";
    std::filesystem::remove_all(root);
    const auto recovery = root / "recovery";

    spark::RecoveryWriter::Config writer_config;
    writer_config.directory = recovery;
    writer_config.session_id = 123456;
    spark::RecoveryWriter writer(writer_config);
    assert(writer.start());
    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {});
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 1, "Server thread");
    spark::Sample sample;
    sample.thread_id = 1;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writer.journalSample(sample);
    writer.stop();

    for (int suffix = 0; suffix < 1000; ++suffix) {
        std::string name = "profile-123456";
        if (suffix != 0) {
            name += '-' + std::to_string(suffix);
        }
        std::ofstream(root / (name + ".sparkprofile"));
    }

    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    TestNotifier notifier;
    spark::SparkApplication application({}, root, root / "activity.json", std::move(config), std::move(trusted),
                                        dispatcher, metadata, notifier);
    application.enable();
    application.shutdown();

    assert(std::filesystem::exists(recovery / "segment-0.jnl"));
    assert(!notifier.messages.empty());
    assert(notifier.messages.back().find("journal retained") != std::string::npos);
    std::filesystem::remove_all(root);
    return 0;
}
