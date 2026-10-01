#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "application/spark_application.h"
#include "core/recovery/recovery_writer.h"
#include "core/recovery/replay_guard.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace spark {

struct SparkApplicationRecoveryTestAccess {
    using Hook = SparkApplication::RecoveryTestHook;

    static void setHook(SparkApplication &application, Hook hook) { application.recovery_test_hook_ = std::move(hook); }
};

}  // namespace spark

namespace {

class TestDispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override { task(); }
};

class TestMetadataProvider final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ServerMetadata &, std::int64_t) override {}
    void gatherWorldMetadata(spark::WorldInfo &, std::string_view) override {}
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }
};

class CountingNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string &, const std::string &text) override { messages.push_back(text); }
    std::vector<std::string> messages;
};

using RecoveryHook =
    std::function<std::error_code(std::string_view, const std::filesystem::path &, const std::filesystem::path &)>;

void writeJournal(const std::filesystem::path &recovery)
{
    spark::RecoveryWriter::Config config;
    config.directory = recovery;
    config.session_id = 246813;
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0);
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 1, "Server thread");
    spark::Sample sample;
    sample.thread_id = 1;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writer.journalSample(sample);
    writer.stop();
}

std::vector<std::filesystem::path> profileFiles(const std::filesystem::path &root)
{
    std::vector<std::filesystem::path> found;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() == ".sparkprofile") {
            found.push_back(entry.path());
        }
    }
    return found;
}

RecoveryHook countReplayAndFailCleanup(int &replay_count, bool fail_cleanup)
{
    return [&replay_count, fail_cleanup](std::string_view operation, const std::filesystem::path &,
                                         const std::filesystem::path &) {
        if (operation == "replay") {
            ++replay_count;
        }
        if (fail_cleanup &&
            (operation == "discard-remove" || operation == "quarantine-rename" || operation == "quarantine-remove")) {
            return std::make_error_code(std::errc::permission_denied);
        }
        return std::error_code{};
    };
}

void startApplication(const std::filesystem::path &root, CountingNotifier &notifier, RecoveryHook hook)
{
    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    spark::SparkApplication application({}, root, root / "activity.json", std::move(config), std::move(trusted),
                                        dispatcher, metadata, notifier);
    spark::SparkApplicationRecoveryTestAccess::setHook(application, std::move(hook));
    application.enable();
    application.shutdown();
}

bool hasMessage(const CountingNotifier &notifier, std::string_view fragment)
{
    return std::ranges::any_of(notifier.messages, [fragment](const std::string &message) {
        return message.find(fragment) != std::string::npos;
    });
}

std::vector<char> readFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void testGuardHelpers()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_guard_helper_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto recovery = root / "recovery";
    const auto guard = spark::recoveryReplayGuardPath(recovery);
    assert(guard == root / "recovery.replay-blocked");
    assert(spark::isRecoveryArtifact("segment-nonnumeric.jnl"));
    assert(spark::isRecoveryArtifact("segment-.jnl.tmp"));
    assert(spark::isRecoveryArtifact("metadata.snapshot"));
    assert(spark::isRecoveryArtifact("metadata.snapshot.tmp"));
    assert(!spark::isRecoveryArtifact("profile.sparkprofile"));

    std::error_code error;
    assert(!spark::recoveryReplayBlocked(recovery, error));
    assert(!error);
    assert(spark::blockRecoveryReplay(recovery, error));
    assert(!error);
    assert(fs::exists(guard));
    assert(spark::recoveryReplayBlocked(recovery, error));
    assert(!error);
    {
        std::ofstream malformed_marker(guard, std::ios::binary | std::ios::trunc);
        malformed_marker << "unreadable marker payload is not authority";
    }
    assert(spark::recoveryReplayBlocked(recovery, error));
    assert(!error);
    assert(spark::clearRecoveryReplayBlock(recovery, error));
    assert(!error);
    assert(!fs::exists(guard));
    fs::create_directory(guard);
    assert(spark::recoveryReplayBlocked(recovery, error));
    assert(!error);
    assert(spark::clearRecoveryReplayBlock(recovery, error));
    assert(!error);
    assert(!fs::exists(guard));
    fs::remove_all(root);
}

void testSuccessfulRecoveryIsDiscarded()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_replay_loop_control";
    fs::remove_all(root);
    const auto recovery = root / "recovery";
    fs::create_directories(recovery);
    writeJournal(recovery);

    int replay_count = 0;
    CountingNotifier notifier;
    startApplication(root, notifier, countReplayAndFailCleanup(replay_count, false));
    std::error_code error;
    assert(replay_count == 1);
    assert(profileFiles(root).size() == 1);
    assert(!fs::exists(recovery / "segment-0.jnl"));
    assert(notifier.messages.size() == 1);
    assert(!spark::recoveryReplayBlocked(recovery, error));
    assert(!error);
    fs::remove_all(root);
}

void testRepeatedCleanupFailureIsBlockedAndFreshWriterResets()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_replay_loop_test";
    fs::remove_all(root);
    const auto recovery = root / "recovery";
    fs::create_directories(recovery);
    writeJournal(recovery);
    const auto guard = spark::recoveryReplayGuardPath(recovery);

    int replay_count = 0;
    CountingNotifier first;
    startApplication(root, first, countReplayAndFailCleanup(replay_count, true));
    std::error_code error;
    assert(replay_count == 1);
    assert(profileFiles(root).size() == 1);
    assert(fs::exists(recovery / "segment-0.jnl"));
    assert(spark::recoveryReplayBlocked(recovery, error));
    assert(!error);
    assert(hasMessage(first, "Recovered profile saved"));
    assert(hasMessage(first, "blocked"));

    CountingNotifier second;
    startApplication(root, second, countReplayAndFailCleanup(replay_count, true));
    assert(replay_count == 1);
    assert(profileFiles(root).size() == 1);
    assert(fs::exists(recovery / "segment-0.jnl"));
    assert(fs::exists(guard));
    assert(hasMessage(second, "blocked"));
    assert(second.messages.size() <= 2);

    // The writer starts a new generation with the same session ID after the old
    // generation's cleanup obstacle is gone.
    writeJournal(recovery);
    assert(!fs::exists(guard));
    assert(fs::exists(recovery / "segment-0.jnl"));

    CountingNotifier third;
    startApplication(root, third, countReplayAndFailCleanup(replay_count, false));
    assert(replay_count == 2);
    assert(profileFiles(root).size() == 2);
    assert(!fs::exists(recovery / "segment-0.jnl"));
    fs::remove_all(root);
}

void testMalformedJournalIsGuardedWithoutIdentity()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_replay_loop_malformed";
    fs::remove_all(root);
    const auto recovery = root / "recovery";
    fs::create_directories(recovery);
    {
        std::ofstream malformed(recovery / "segment-not-a-number.jnl", std::ios::binary);
        malformed << "short";
    }

    int replay_count = 0;
    CountingNotifier first;
    startApplication(root, first, countReplayAndFailCleanup(replay_count, true));
    std::error_code error;
    assert(replay_count == 1);
    assert(fs::exists(recovery / "segment-not-a-number.jnl"));
    assert(spark::recoveryReplayBlocked(recovery, error));
    assert(!error);

    CountingNotifier second;
    startApplication(root, second, countReplayAndFailCleanup(replay_count, true));
    assert(replay_count == 1);
    assert(fs::exists(recovery / "segment-not-a-number.jnl"));
    assert(hasMessage(second, "blocked"));
    fs::remove_all(root);
}

void testPartialCleanupLeavesChangedRemainderBlocked()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_replay_loop_partial_cleanup";
    fs::remove_all(root);
    const auto recovery = root / "recovery";
    fs::create_directories(recovery);
    writeJournal(recovery);

    int replay_count = 0;
    bool partial_cleanup_injected = false;
    RecoveryHook hook = [&replay_count, &partial_cleanup_injected](std::string_view operation, const fs::path &path,
                                                                   const fs::path &) {
        if (operation == "replay") {
            ++replay_count;
        }
        if (operation == "discard-remove") {
            if (!partial_cleanup_injected) {
                std::error_code error;
                fs::remove(path / "segment-0.jnl", error);
                assert(!error);
                std::ofstream(path / "segment-1.jnl", std::ios::binary) << "remaining journal fragment";
                partial_cleanup_injected = true;
            }
            return std::make_error_code(std::errc::permission_denied);
        }
        if (operation == "quarantine-rename" || operation == "quarantine-remove") {
            return std::make_error_code(std::errc::permission_denied);
        }
        return std::error_code{};
    };

    CountingNotifier first;
    startApplication(root, first, hook);
    std::error_code error;
    assert(replay_count == 1);
    assert(profileFiles(root).size() == 1);
    assert(!fs::exists(recovery / "segment-0.jnl"));
    assert(fs::exists(recovery / "segment-1.jnl"));
    assert(spark::recoveryReplayBlocked(recovery, error));
    assert(!error);
    assert(hasMessage(first, "blocked"));

    CountingNotifier second;
    startApplication(root, second, hook);
    assert(replay_count == 1);
    assert(profileFiles(root).size() == 1);
    assert(fs::exists(recovery / "segment-1.jnl"));
    assert(hasMessage(second, "blocked"));
    fs::remove_all(root);
}

void testGuardPublishFailureRetainsJournal()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_replay_loop_guard_publish_failure";
    fs::remove_all(root);
    const auto recovery = root / "recovery";
    fs::create_directories(recovery);
    writeJournal(recovery);

    int replay_count = 0;
    int cleanup_calls = 0;
    RecoveryHook hook = [&replay_count, &cleanup_calls](std::string_view operation, const fs::path &,
                                                        const fs::path &) {
        if (operation == "replay") {
            ++replay_count;
        }
        if (operation == "guard-publish") {
            return std::make_error_code(std::errc::permission_denied);
        }
        if (operation == "discard-remove" || operation == "quarantine-rename" || operation == "quarantine-remove") {
            ++cleanup_calls;
            return std::make_error_code(std::errc::permission_denied);
        }
        return std::error_code{};
    };

    CountingNotifier notifier;
    startApplication(root, notifier, std::move(hook));
    std::error_code error;
    assert(replay_count == 1);
    assert(cleanup_calls == 0);
    assert(profileFiles(root).size() == 1);
    assert(fs::exists(recovery / "segment-0.jnl"));
    assert(!spark::recoveryReplayBlocked(recovery, error));
    assert(!error);
    assert(hasMessage(notifier, "Could not persist the replay guard"));
    assert(hasMessage(notifier, "may happen again after restart"));
    fs::remove_all(root);
}

void testFreshDirectoryFailureIsReportedAfterPurge()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_replay_loop_create_failure";
    fs::remove_all(root);
    const auto recovery = root / "recovery";
    fs::create_directories(recovery);
    writeJournal(recovery);

    int replay_count = 0;
    RecoveryHook hook = [&replay_count](std::string_view operation, const fs::path &, const fs::path &) {
        if (operation == "replay") {
            ++replay_count;
        }
        if (operation == "create-directory") {
            return std::make_error_code(std::errc::permission_denied);
        }
        return std::error_code{};
    };

    CountingNotifier notifier;
    startApplication(root, notifier, std::move(hook));
    std::error_code error;
    assert(replay_count == 1);
    assert(profileFiles(root).size() == 1);
    assert(!fs::exists(recovery / "segment-0.jnl"));
    assert(!fs::exists(recovery));
    assert(!spark::recoveryReplayBlocked(recovery, error));
    assert(!error);
    assert(hasMessage(notifier, "fresh recovery directory could not be created"));
    fs::remove_all(root);
}

void testGuardRetirementFailureKeepsBlock()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_replay_loop_guard_clear_failure";
    fs::remove_all(root);
    const auto recovery = root / "recovery";
    fs::create_directories(recovery);
    writeJournal(recovery);

    int replay_count = 0;
    RecoveryHook hook = [&replay_count](std::string_view operation, const fs::path &, const fs::path &) {
        if (operation == "replay") {
            ++replay_count;
        }
        if (operation == "guard-clear") {
            return std::make_error_code(std::errc::permission_denied);
        }
        return std::error_code{};
    };

    CountingNotifier notifier;
    startApplication(root, notifier, std::move(hook));
    std::error_code error;
    assert(replay_count == 1);
    assert(profileFiles(root).size() == 1);
    assert(!fs::exists(recovery / "segment-0.jnl"));
    assert(spark::recoveryReplayBlocked(recovery, error));
    assert(!error);
    assert(hasMessage(notifier, "replay guard could not be retired"));
    fs::remove_all(root);
}

void testGuardProbeFailureSkipsReplay()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_replay_loop_guard_probe_failure";
    fs::remove_all(root);
    const auto recovery = root / "recovery";
    fs::create_directories(recovery);
    writeJournal(recovery);

    int replay_count = 0;
    int cleanup_calls = 0;
    RecoveryHook hook = [&replay_count, &cleanup_calls](std::string_view operation, const fs::path &,
                                                        const fs::path &) {
        if (operation == "replay") {
            ++replay_count;
        }
        if (operation == "discard-remove" || operation == "quarantine-rename" || operation == "quarantine-remove" ||
            operation == "create-directory") {
            ++cleanup_calls;
        }
        if (operation == "guard-probe") {
            return std::make_error_code(std::errc::permission_denied);
        }
        return std::error_code{};
    };

    CountingNotifier notifier;
    startApplication(root, notifier, std::move(hook));
    assert(replay_count == 0);
    assert(cleanup_calls == 0);
    assert(profileFiles(root).empty());
    assert(fs::exists(recovery / "segment-0.jnl"));
    assert(hasMessage(notifier, "replay guard could not be inspected"));
    assert(hasMessage(notifier, "Recovery artifacts may remain"));
    fs::remove_all(root);
}

void guardProbeOnlyDataLossRepro()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_guard_probe_only";
    fs::remove_all(root);
    const auto recovery = root / "recovery";
    fs::create_directories(recovery);
    const auto journal = recovery / "segment-0.jnl";
    writeJournal(recovery);
    const auto original_bytes = readFile(journal);

    int replay_count = 0;
    int cleanup_calls = 0;
    RecoveryHook uncertain_probe = [&replay_count, &cleanup_calls](std::string_view operation, const fs::path &,
                                                                   const fs::path &) {
        if (operation == "replay") {
            ++replay_count;
        }
        if (operation == "discard-remove" || operation == "quarantine-rename" || operation == "quarantine-remove" ||
            operation == "create-directory") {
            ++cleanup_calls;
        }
        if (operation == "guard-probe") {
            return std::make_error_code(std::errc::permission_denied);
        }
        return std::error_code{};
    };

    CountingNotifier first;
    startApplication(root, first, std::move(uncertain_probe));
    const bool journal_exists_after_first = fs::exists(journal);
    const bool bytes_unchanged_after_first = journal_exists_after_first && readFile(journal) == original_bytes;
    const auto first_profile_count = profileFiles(root).size();
    const auto first_replay_count = replay_count;
    std::fprintf(stderr,
                 "guard-probe-only app1: replay_count=%d cleanup_calls=%d journal_exists=%d bytes_unchanged=%d "
                 "profile_count=%zu\n",
                 first_replay_count, cleanup_calls, static_cast<int>(journal_exists_after_first),
                 static_cast<int>(bytes_unchanged_after_first), first_profile_count);
    std::fflush(stderr);

    CountingNotifier second;
    startApplication(root, second, [&replay_count](std::string_view operation, const fs::path &, const fs::path &) {
        if (operation == "replay") {
            ++replay_count;
        }
        return std::error_code{};
    });
    const auto second_profile_count = profileFiles(root).size();
    const auto second_replay_delta = replay_count - first_replay_count;
    std::fprintf(stderr, "guard-probe-only app2 after probe repair: replay_delta=%d profile_count=%zu\n",
                 second_replay_delta, second_profile_count);
    std::fflush(stderr);

    const bool recovered_after_probe_repair = second_replay_delta == 1 && second_profile_count == 1;
    fs::remove_all(root);
    assert(first_replay_count == 0);
    assert(cleanup_calls == 0);
    assert(journal_exists_after_first);
    assert(bytes_unchanged_after_first);
    assert(first_profile_count == 0);
    assert(hasMessage(first, "replay guard could not be inspected"));
    assert(recovered_after_probe_repair);
}

#ifdef _WIN32
void testWindowsHeldFileBlocksCleanup()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_replay_loop_windows_lock";
    fs::remove_all(root);
    const auto recovery = root / "recovery";
    fs::create_directories(recovery);
    writeJournal(recovery);
    const auto held = recovery / "segment-0.jnl";

    // Allow replay reads while denying delete-sharing for deterministic cleanup failure.
    HANDLE lock = ::CreateFileW(held.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(lock != INVALID_HANDLE_VALUE);

    int replay_count = 0;
    CountingNotifier first;
    startApplication(root, first, countReplayAndFailCleanup(replay_count, false));
    std::error_code error;
    assert(replay_count == 1);
    assert(profileFiles(root).size() == 1);
    assert(fs::exists(held));
    assert(spark::recoveryReplayBlocked(recovery, error));
    assert(!error);
    assert(hasMessage(first, "Quarantining recovery journal"));
    assert(hasMessage(first, "blocked"));

    CountingNotifier second;
    startApplication(root, second, countReplayAndFailCleanup(replay_count, false));
    assert(replay_count == 1);
    assert(profileFiles(root).size() == 1);
    assert(fs::exists(held));
    assert(fs::exists(spark::recoveryReplayGuardPath(recovery)));
    assert(hasMessage(second, "blocked"));
    assert(second.messages.size() <= 4);

    ::CloseHandle(lock);
    fs::remove_all(root);
}
#endif

void crossStartProbe()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "spark_recovery_replay_cross_start_probe";
    fs::remove_all(root);
    const auto recovery = root / "recovery";
    fs::create_directories(recovery);
    writeJournal(recovery);

    int replay_count = 0;
    CountingNotifier first;
    startApplication(root, first, countReplayAndFailCleanup(replay_count, true));
    const auto first_profile_count = profileFiles(root).size();
    std::fprintf(stderr, "cross-start app1: replay_count=%d profile_count=%zu\n", replay_count, first_profile_count);
    std::fflush(stderr);

    CountingNotifier second;
    startApplication(root, second, countReplayAndFailCleanup(replay_count, true));
    const auto second_profile_count = profileFiles(root).size();
    std::fprintf(stderr, "cross-start app2: replay_count=%d profile_count=%zu\n", replay_count, second_profile_count);
    std::fflush(stderr);

    fs::remove_all(root);
    assert(replay_count == 1);
    assert(second_profile_count == first_profile_count);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--guard-probe-only") {
        guardProbeOnlyDataLossRepro();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--cross-start-probe") {
        crossStartProbe();
        return 0;
    }
    testGuardHelpers();
    testSuccessfulRecoveryIsDiscarded();
    testRepeatedCleanupFailureIsBlockedAndFreshWriterResets();
    testMalformedJournalIsGuardedWithoutIdentity();
    testPartialCleanupLeavesChangedRemainderBlocked();
    testGuardPublishFailureRetainsJournal();
    testFreshDirectoryFailureIsReportedAfterPurge();
    testGuardRetirementFailureKeepsBlock();
    testGuardProbeFailureSkipsReplay();
    guardProbeOnlyDataLossRepro();
#ifdef _WIN32
    testWindowsHeldFileBlocksCleanup();
#endif
    std::cout << "All recovery replay-loop tests passed.\n";
    return 0;
}
