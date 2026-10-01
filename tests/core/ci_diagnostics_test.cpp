#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "native/diagnostics/ci_diagnostics.h"
#include "native/diagnostics/ci_diagnostics_snapshot.h"

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
// clang-format on
#endif

namespace {

using spark::CiDiagnosticContext;
using spark::CiDiagnosticCounter;
using spark::CiDiagnosticPhase;
using spark::CiDiagnosticRecord;
using spark::CiDiagnostics;
using spark::CiDiagnosticSnapshot;
using spark::CiDiagnosticsRegion;

bool require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "ci diagnostics: %s\n", message);
    }
    return condition;
}

bool isUnknown(const CiDiagnosticSnapshot &snapshot)
{
    return !snapshot.consistent && snapshot.sequence == 0 && snapshot.generation == 0 &&
           snapshot.phase == CiDiagnosticPhase::Unknown && snapshot.transition == 0 && snapshot.worker_tid == 0 &&
           snapshot.target_tid == 0 && snapshot.suspend_success_count == 0 && snapshot.resume_success_count == 0 &&
           snapshot.walk_call_count == 0;
}

class EnvironmentGuard final {
public:
    EnvironmentGuard() : had_value_(std::getenv("ENDSTONE_SPARK_CI_DIAGNOSTICS") != nullptr)
    {
        if (had_value_) {
            previous_value_ = std::getenv("ENDSTONE_SPARK_CI_DIAGNOSTICS");
        }
    }

    ~EnvironmentGuard()
    {
#ifdef _WIN32
        if (had_value_) {
            _putenv_s("ENDSTONE_SPARK_CI_DIAGNOSTICS", previous_value_.c_str());
        }
        else {
            _putenv_s("ENDSTONE_SPARK_CI_DIAGNOSTICS", "");
        }
#else
        if (had_value_) {
            setenv("ENDSTONE_SPARK_CI_DIAGNOSTICS", previous_value_.c_str(), 1);
        }
        else {
            unsetenv("ENDSTONE_SPARK_CI_DIAGNOSTICS");
        }
#endif
    }

    static bool set(const char *value)
    {
#ifdef _WIN32
        return _putenv_s("ENDSTONE_SPARK_CI_DIAGNOSTICS", value != nullptr ? value : "") == 0;
#else
        return value != nullptr ? setenv("ENDSTONE_SPARK_CI_DIAGNOSTICS", value, 1) == 0
                                : unsetenv("ENDSTONE_SPARK_CI_DIAGNOSTICS") == 0;
#endif
    }

private:
    bool had_value_;
    std::string previous_value_;
};

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
class MappingViewGuard final {
public:
    ~MappingViewGuard()
    {
        if (view_ != nullptr) {
            ::UnmapViewOfFile(view_);
        }
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
        }
    }

    bool open(std::string_view name)
    {
        const std::wstring wide_name(name.begin(), name.end());
        handle_ = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide_name.c_str());
        if (handle_ == nullptr) {
            return false;
        }
        view_ = ::MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CiDiagnosticsRegion));
        if (view_ == nullptr) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
            return false;
        }
        return true;
    }

    [[nodiscard]] CiDiagnosticsRegion *region() const { return static_cast<CiDiagnosticsRegion *>(view_); }

private:
    HANDLE handle_ = nullptr;
    void *view_ = nullptr;
};
#endif

bool testDisabledAndSchema()
{
    if (!require(!CiDiagnostics::environmentEnabled(nullptr), "null environment value enabled diagnostics") ||
        !require(!CiDiagnostics::environmentEnabled(""), "empty environment value enabled diagnostics") ||
        !require(!CiDiagnostics::environmentEnabled("0"), "zero environment value enabled diagnostics") ||
        !require(!CiDiagnostics::environmentEnabled("10"), "multi-character environment value enabled diagnostics") ||
        !require(CiDiagnostics::environmentEnabled("1"), "one environment value did not enable diagnostics") ||
        !require(CiDiagnostics::mappingNameForPid(42) == "Local\\EndstoneSparkCiDiag-v2-42",
                 "mapping name schema changed")) {
        return false;
    }

    static_assert(offsetof(CiDiagnosticsRegion, records) == 5 * sizeof(std::uint64_t));
    static_assert(offsetof(CiDiagnosticRecord, generation) == sizeof(std::uint64_t));
    static_assert(sizeof(CiDiagnosticRecord) == 8 * sizeof(std::uint64_t));
    static_assert(sizeof(CiDiagnosticsRegion) == (5 + spark::kCiDiagnosticsRecordCount * 8) * sizeof(std::uint64_t));

    CiDiagnostics disabled;
    if (!require(disabled.openForTesting(false), "disabled test diagnostics failed to open") ||
        !require(!disabled.enabled(), "disabled test diagnostics reported enabled") ||
        !require(spark::globalCiDiagnostics() == nullptr, "disabled diagnostics registered globally")) {
        return false;
    }
    const auto *disabled_region = disabled.regionForTesting();
    if (!require(disabled_region->magic.load() == spark::kCiDiagnosticsMagic, "disabled schema magic is wrong") ||
        !require(disabled_region->version.load() == spark::kCiDiagnosticsVersion, "disabled schema version is wrong") ||
        !require(disabled_region->size.load() == sizeof(CiDiagnosticsRegion), "disabled schema size is wrong") ||
        !require(disabled_region->ready.load() == 0, "disabled diagnostics published readiness")) {
        return false;
    }
    disabled.publish(CiDiagnosticContext::ApplicationCommand, CiDiagnosticPhase::ApplicationCommandEnter, 1, 2);
    if (!require(disabled_region->records[static_cast<std::size_t>(CiDiagnosticContext::ApplicationCommand)]
                         .sequence.load() == 0,
                 "disabled diagnostics accepted a publication")) {
        return false;
    }
    disabled.close();

    CiDiagnostics enabled;
    if (!require(enabled.openForTesting(true), "enabled test diagnostics failed to open") ||
        !require(enabled.enabled(), "enabled test diagnostics reported disabled") ||
        !require(spark::globalCiDiagnostics() == &enabled, "enabled diagnostics was not registered globally")) {
        return false;
    }
    const auto *region = enabled.regionForTesting();
    return require(region->magic.load() == spark::kCiDiagnosticsMagic, "schema magic is wrong") &&
           require(region->version.load() == spark::kCiDiagnosticsVersion, "schema version is wrong") &&
           require(region->size.load() == sizeof(CiDiagnosticsRegion), "schema size is wrong") &&
           require(region->ready.load() == spark::kCiDiagnosticsReady, "schema was not published ready");
}

bool testEnvironmentOptIn()
{
    EnvironmentGuard environment;
    if (!require(EnvironmentGuard::set("1"), "failed to set diagnostics environment")) {
        return false;
    }

    CiDiagnostics diagnostics;
    if (!require(diagnostics.open(), "environment diagnostics failed to open")) {
        return false;
    }
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const bool enabled = diagnostics.enabled();
    const bool registered = spark::globalCiDiagnostics() == &diagnostics;
    diagnostics.close();
    return require(enabled, "Windows environment opt-in remained disabled") &&
           require(registered, "Windows environment opt-in did not register") &&
           require(spark::globalCiDiagnostics() == nullptr, "closed environment diagnostics remained registered");
#else
    return require(!diagnostics.enabled(), "non-Windows environment opt-in enabled diagnostics") &&
           require(spark::globalCiDiagnostics() == nullptr, "non-Windows environment opt-in registered diagnostics");
#endif
}

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
bool testMappingOwnershipAndReopen()
{
    EnvironmentGuard environment;
    if (!require(EnvironmentGuard::set("1"), "failed to set diagnostics environment for mapping test")) {
        return false;
    }

    CiDiagnostics first;
    if (!require(first.open(), "first mapped diagnostics failed to open") ||
        !require(first.enabled(), "first mapped diagnostics remained disabled") ||
        !require(spark::globalCiDiagnostics() == &first, "first mapped diagnostics was not registered")) {
        return false;
    }
    auto *first_region = first.regionForTesting();
    const std::uint64_t first_mapping_generation = first_region->generation.load(std::memory_order_seq_cst);
    if (!require(first_region->ready.load(std::memory_order_seq_cst) == spark::kCiDiagnosticsReady,
                 "fresh mapping was not published ready") ||
        !require(first_mapping_generation != 0, "fresh mapping lifetime generation was zero")) {
        return false;
    }
    first.publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureEnter, 11, 22);

    MappingViewGuard retained;
    if (!require(retained.open(first.mappingName()), "failed to retain mapped diagnostics externally")) {
        return false;
    }
    auto *retained_region = retained.region();
    if (!require(retained_region->ready.load(std::memory_order_seq_cst) == spark::kCiDiagnosticsReady,
                 "retained mapping was not ready before overlap")) {
        return false;
    }

    CiDiagnostics overlap;
    if (!require(!overlap.open(), "overlapping diagnostics owner was accepted") ||
        !require(spark::globalCiDiagnostics() == &first, "overlap changed the global owner") ||
        !require(first_region->ready.load(std::memory_order_seq_cst) == spark::kCiDiagnosticsReady,
                 "overlap cleared incumbent readiness") ||
        !require(retained_region->ready.load(std::memory_order_seq_cst) == spark::kCiDiagnosticsReady,
                 "overlap changed retained readiness")) {
        return false;
    }

    spark::unregisterGlobalCiDiagnostics(&first);
    CiDiagnostics in_use;
    if (!require(!in_use.open(), "active retained mapping was accepted after owner pointer removal") ||
        !require(first_region->ready.load(std::memory_order_seq_cst) == spark::kCiDiagnosticsReady,
                 "active retained rejection cleared incumbent readiness") ||
        !require(spark::registerGlobalCiDiagnostics(&first), "failed to restore incumbent registration")) {
        return false;
    }

    first.close();
    if (!require(spark::globalCiDiagnostics() == nullptr, "first close did not clear global owner") ||
        !require(retained_region->ready.load(std::memory_order_seq_cst) == 0,
                 "first close did not clear owned mapping readiness")) {
        return false;
    }

    CiDiagnostics second;
    if (!require(second.open(), "retained diagnostics failed to reopen") ||
        !require(second.enabled(), "retained diagnostics remained disabled") ||
        !require(spark::globalCiDiagnostics() == &second, "retained diagnostics was not registered")) {
        return false;
    }
    auto *second_region = second.regionForTesting();
    const auto retained_record =
        readCiDiagnosticSnapshot(second_region->records[static_cast<std::size_t>(CiDiagnosticContext::Capture)]);
    const std::uint64_t second_mapping_generation = second_region->generation.load(std::memory_order_seq_cst);
    if (!require(second_mapping_generation == first_mapping_generation + 1,
                 "retained mapping lifetime generation did not advance") ||
        !require(second.beginGeneration() == 1, "profiler generation did not start independently") ||
        !require(second_region->generation.load(std::memory_order_seq_cst) == second_mapping_generation,
                 "profiler generation changed mapping lifetime generation") ||
        !require(retained_region->ready.load(std::memory_order_seq_cst) == spark::kCiDiagnosticsReady,
                 "reopened mapping was not ready") ||
        !require(retained_record.consistent && retained_record.sequence != 0 && retained_record.generation == 0 &&
                     retained_record.phase == CiDiagnosticPhase::Unknown && retained_record.transition == 0 &&
                     retained_record.worker_tid == 0 && retained_record.target_tid == 0 &&
                     retained_record.suspend_success_count == 0 && retained_record.resume_success_count == 0 &&
                     retained_record.walk_call_count == 0,
                 "retained mapping record was not invalidated")) {
        return false;
    }

    second.publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureEnter, 33, 44);
    const auto fresh_record =
        readCiDiagnosticSnapshot(second_region->records[static_cast<std::size_t>(CiDiagnosticContext::Capture)]);
    if (!require(fresh_record.consistent && fresh_record.generation == 1 &&
                     fresh_record.phase == CiDiagnosticPhase::CaptureEnter && fresh_record.worker_tid == 33 &&
                     fresh_record.target_tid == 44,
                 "fresh reopened mapping publication was not attributed correctly")) {
        return false;
    }

    second.close();
    if (!require(retained_region->ready.load(std::memory_order_seq_cst) == 0,
                 "second close did not clear owned mapping readiness") ||
        !require(spark::globalCiDiagnostics() == nullptr, "second close left a global owner")) {
        return false;
    }

    retained_region->generation.store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_seq_cst);
    CiDiagnostics wrapped;
    if (!require(wrapped.open(), "mapping lifetime generation wrap failed") ||
        !require(wrapped.regionForTesting()->generation.load(std::memory_order_seq_cst) == 1,
                 "mapping lifetime generation wrapped through zero") ||
        !require(retained_region->ready.load(std::memory_order_seq_cst) == spark::kCiDiagnosticsReady,
                 "wrapped mapping was not published ready")) {
        return false;
    }
    wrapped.close();

    const std::uint64_t malformed_magic = retained_region->magic.load(std::memory_order_seq_cst);
    retained_region->magic.store(0, std::memory_order_seq_cst);
    CiDiagnostics malformed;
    const bool rejected = !malformed.open();
    const bool unchanged = retained_region->magic.load(std::memory_order_seq_cst) == 0 &&
                           retained_region->ready.load(std::memory_order_seq_cst) == 0;
    retained_region->magic.store(malformed_magic, std::memory_order_seq_cst);
    return require(rejected, "malformed retained mapping was accepted") &&
           require(unchanged, "malformed retained mapping was mutated");
}
#endif

bool testInvalidContextAndCoherentPublication()
{
    CiDiagnostics diagnostics;
    if (!require(diagnostics.openForTesting(true), "coherence diagnostics failed to open")) {
        return false;
    }
    auto *region = diagnostics.regionForTesting();
    const auto invalid_count = static_cast<CiDiagnosticContext>(CiDiagnosticContext::Count);
    diagnostics.publish(invalid_count, CiDiagnosticPhase::ApplicationCommandEnter, 1, 2);
    diagnostics.publish(static_cast<CiDiagnosticContext>(std::numeric_limits<std::uint64_t>::max()),
                        CiDiagnosticPhase::ApplicationCommandExit, 3, 4);
    for (const auto &record : region->records) {
        if (!require(record.sequence.load() == 0, "invalid context modified a record")) {
            return false;
        }
    }

    if (!require(diagnostics.beginGeneration() == 1, "first generation was not one") ||
        !require(diagnostics.generation() == 1, "generation accessor returned the wrong value")) {
        return false;
    }
    diagnostics.publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureSuspendAttempt, 11, 22,
                        CiDiagnosticCounter::SuspendSuccess, 2);
    const auto &record = region->records[static_cast<std::size_t>(CiDiagnosticContext::Capture)];
    const CiDiagnosticSnapshot first = spark::readCiDiagnosticSnapshot(record);
    if (!require(first.consistent, "first publication was not coherent") ||
        !require(first.sequence == 2 && first.generation == 1, "first publication sequence or generation is wrong") ||
        !require(first.phase == CiDiagnosticPhase::CaptureSuspendAttempt && first.transition == 1,
                 "first publication phase or transition is wrong") ||
        !require(first.worker_tid == 11 && first.target_tid == 22, "first publication thread ids are wrong") ||
        !require(first.suspend_success_count == 2 && first.resume_success_count == 0 && first.walk_call_count == 0,
                 "first publication counters are wrong")) {
        return false;
    }

    diagnostics.publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureResumeAttempt, 0, 0,
                        CiDiagnosticCounter::ResumeSuccess, 3);
    const CiDiagnosticSnapshot second = spark::readCiDiagnosticSnapshot(record);
    if (!require(second.consistent && second.sequence == 4 && second.generation == 1,
                 "second publication sequence or generation is wrong") ||
        !require(second.phase == CiDiagnosticPhase::CaptureResumeAttempt && second.transition == 2,
                 "second publication phase or transition is wrong") ||
        !require(second.worker_tid == 11 && second.target_tid == 22, "zero thread ids did not preserve prior values") ||
        !require(second.suspend_success_count == 2 && second.resume_success_count == 3,
                 "second publication counters are wrong")) {
        return false;
    }

    diagnostics.publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureWalkCall, 0, 0,
                        CiDiagnosticCounter::WalkCalls, 4);
    const CiDiagnosticSnapshot third = spark::readCiDiagnosticSnapshot(record);
    if (!require(third.consistent && third.sequence == 6 && third.transition == 3,
                 "third publication sequence or transition is wrong") ||
        !require(third.walk_call_count == 4, "third publication walk counter is wrong") ||
        !require(diagnostics.beginGeneration() == 2 && diagnostics.generation() == 2,
                 "second generation was not published")) {
        return false;
    }
    diagnostics.publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureComplete);
    const CiDiagnosticSnapshot fourth = spark::readCiDiagnosticSnapshot(record);
    return require(fourth.consistent && fourth.generation == 2 && fourth.phase == CiDiagnosticPhase::CaptureComplete,
                   "publication did not carry the new generation");
}

bool testChangedSequenceRejected()
{
    CiDiagnosticRecord record;
    record.sequence.store(2, std::memory_order_seq_cst);
    record.generation.store(7, std::memory_order_seq_cst);
    record.phase_and_transition.store(static_cast<std::uint64_t>(CiDiagnosticPhase::ApplicationCommandEnter),
                                      std::memory_order_seq_cst);
    record.worker_tid.store(8, std::memory_order_seq_cst);
    bool callback_invoked = false;
    const CiDiagnosticSnapshot snapshot = spark::ci_diagnostics_snapshot_detail::readSnapshot(record, [&]() noexcept {
        callback_invoked = true;
        record.sequence.store(4, std::memory_order_seq_cst);
    });
    return require(callback_invoked, "sequence-change callback did not run inside the snapshot read") &&
           require(record.sequence.load(std::memory_order_seq_cst) == 4,
                   "sequence-change callback did not advance the sequence") &&
           require(isUnknown(snapshot), "a changed sequence was accepted as a snapshot");
}

bool testOddSequenceRejected()
{
    CiDiagnosticRecord record;
    record.generation.store(7);
    record.phase_and_transition.store(static_cast<std::uint64_t>(CiDiagnosticPhase::ApplicationCommandEnter));
    record.worker_tid.store(8);
    record.sequence.store(1);
    bool callback_invoked = false;
    const CiDiagnosticSnapshot snapshot =
        spark::ci_diagnostics_snapshot_detail::readSnapshot(record, [&]() noexcept { callback_invoked = true; });
    return require(isUnknown(snapshot), "odd updating sequence was accepted as a snapshot") &&
           require(!callback_invoked, "odd sequence entered the snapshot read window");
}

bool testScopeAndRegistration()
{
    CiDiagnostics diagnostics;
    if (!require(diagnostics.openForTesting(true), "scope diagnostics failed to open")) {
        return false;
    }
    {
        CiDiagnostics::Scope scope(
            &diagnostics, CiDiagnosticContext::ApplicationCommand, CiDiagnosticPhase::ApplicationCommandEnter,
            CiDiagnosticPhase::ApplicationCommandExit, CiDiagnosticPhase::ApplicationCommandExceptionalExit, 31, 41);
    }
    const auto &record =
        diagnostics.regionForTesting()->records[static_cast<std::size_t>(CiDiagnosticContext::ApplicationCommand)];
    if (!require(spark::readCiDiagnosticSnapshot(record).phase == CiDiagnosticPhase::ApplicationCommandExit,
                 "scope did not publish normal exit")) {
        return false;
    }
    bool caught = false;
    try {
        CiDiagnostics::Scope scope(
            &diagnostics, CiDiagnosticContext::ApplicationCommand, CiDiagnosticPhase::ApplicationCommandEnter,
            CiDiagnosticPhase::ApplicationCommandExit, CiDiagnosticPhase::ApplicationCommandExceptionalExit);
        throw 1;
    }
    catch (...) {
        caught = true;
    }
    if (!require(caught, "scope did not throw") ||
        !require(spark::readCiDiagnosticSnapshot(record).phase == CiDiagnosticPhase::ApplicationCommandExceptionalExit,
                 "scope did not publish exceptional exit")) {
        return false;
    }
    diagnostics.close();

    CiDiagnostics first;
    CiDiagnostics second;
    if (!require(spark::registerGlobalCiDiagnostics(&first), "first global registration failed") ||
        !require(spark::globalCiDiagnostics() == &first, "first global registration was not visible") ||
        !require(!spark::registerGlobalCiDiagnostics(&second), "second global registration was accepted") ||
        !require(spark::globalCiDiagnostics() == &first, "failed registration replaced the active instance")) {
        spark::unregisterGlobalCiDiagnostics(&first);
        return false;
    }
    spark::unregisterGlobalCiDiagnostics(&second);
    spark::unregisterGlobalCiDiagnostics(&first);
    if (!require(spark::globalCiDiagnostics() == nullptr, "global unregister did not clear the instance") ||
        !require(spark::registerGlobalCiDiagnostics(&second), "sequential global replacement failed") ||
        !require(spark::globalCiDiagnostics() == &second, "replacement instance was not visible")) {
        spark::unregisterGlobalCiDiagnostics(&second);
        return false;
    }
    spark::unregisterGlobalCiDiagnostics(&second);
    return require(spark::globalCiDiagnostics() == nullptr, "replacement unregister did not clear the instance");
}

}  // namespace

int main()
{
    bool ok = testDisabledAndSchema() && testEnvironmentOptIn();
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    ok = ok && testMappingOwnershipAndReopen();
#endif
    ok = ok && testInvalidContextAndCoherentPublication() && testOddSequenceRejected() &&
         testChangedSequenceRejected() && testScopeAndRegistration();
    if (ok) {
        std::puts("All CI diagnostics tests passed.");
    }
    return ok ? 0 : 1;
}
