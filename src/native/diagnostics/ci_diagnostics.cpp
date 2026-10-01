#include "native/diagnostics/ci_diagnostics.h"

#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

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

namespace spark {
namespace {

std::atomic<CiDiagnostics *> GDiagnostics{nullptr};
thread_local bool GLiveExportScope = false;

void clearRecord(CiDiagnosticRecord &record) noexcept
{
    record.sequence.store(0, std::memory_order_relaxed);
    record.generation.store(0, std::memory_order_relaxed);
    record.phase_and_transition.store(0, std::memory_order_relaxed);
    record.worker_tid.store(0, std::memory_order_relaxed);
    record.target_tid.store(0, std::memory_order_relaxed);
    record.suspend_success_count.store(0, std::memory_order_relaxed);
    record.resume_success_count.store(0, std::memory_order_relaxed);
    record.walk_call_count.store(0, std::memory_order_relaxed);
}

void invalidateRecord(CiDiagnosticRecord &record) noexcept
{
    const std::uint64_t current = record.sequence.load(std::memory_order_seq_cst);
    const std::uint64_t odd = (current & ~1ULL) + 1;
    record.sequence.store(odd, std::memory_order_seq_cst);
    record.generation.store(0, std::memory_order_seq_cst);
    record.phase_and_transition.store(0, std::memory_order_seq_cst);
    record.worker_tid.store(0, std::memory_order_seq_cst);
    record.target_tid.store(0, std::memory_order_seq_cst);
    record.suspend_success_count.store(0, std::memory_order_seq_cst);
    record.resume_success_count.store(0, std::memory_order_seq_cst);
    record.walk_call_count.store(0, std::memory_order_seq_cst);
    record.sequence.store(odd + 1, std::memory_order_seq_cst);
}

std::uint64_t nextTransition(const CiDiagnosticRecord &record) noexcept
{
    const std::uint64_t packed = record.phase_and_transition.load(std::memory_order_seq_cst);
    return ((packed >> ci_diagnostics_snapshot_detail::kTransitionShift) + 1) &
           ci_diagnostics_snapshot_detail::kTransitionMask;
}

}  // namespace

CiDiagnosticSnapshot readCiDiagnosticSnapshot(const CiDiagnosticRecord &record) noexcept
{
    return ci_diagnostics_snapshot_detail::readSnapshot(record, ci_diagnostics_snapshot_detail::NoSequenceReadHook{});
}

CiDiagnostics::CiDiagnostics() noexcept
{
    initializeRegion(local_region_, 0);
    local_region_.ready.store(0, std::memory_order_seq_cst);
}

CiDiagnostics::~CiDiagnostics()
{
    close();
}

bool CiDiagnostics::environmentEnabled(const char *value) noexcept
{
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

std::string CiDiagnostics::mappingNameForPid(std::uint64_t pid)
{
    return "Local\\EndstoneSparkCiDiag-v2-" + std::to_string(pid);
}

bool CiDiagnostics::open() noexcept
{
    try {
        close();
        initializeRegion(local_region_, 0);
        local_region_.ready.store(0, std::memory_order_seq_cst);
        region_ = &local_region_;
        profiler_generation_.store(0, std::memory_order_relaxed);
        if (!environmentEnabled(std::getenv("ENDSTONE_SPARK_CI_DIAGNOSTICS"))) {
            return true;
        }
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        if (!registerGlobalCiDiagnostics(this)) {
            return false;
        }
        registered_owner_ = true;
        if (!openMapping()) {
            close();
            return false;
        }
        return true;
#else
        return true;
#endif
    }
    catch (...) {
        close();
        return false;
    }
}

bool CiDiagnostics::openForTesting(bool enabled) noexcept
{
    close();
    initializeRegion(local_region_, 0);
    region_ = &local_region_;
    profiler_generation_.store(0, std::memory_order_relaxed);
    mapping_name_.clear();
    if (!enabled) {
        region_->ready.store(0, std::memory_order_seq_cst);
        return true;
    }
    enabled_ = true;
    if (!registerGlobalCiDiagnostics(this)) {
        enabled_ = false;
        region_->ready.store(0, std::memory_order_seq_cst);
        return false;
    }
    registered_owner_ = true;
    mapping_owner_ = true;
    return true;
}

void CiDiagnostics::close() noexcept
{
    const bool registered_owner = registered_owner_;
    const bool mapping_owner = mapping_owner_;
    if (registered_owner) {
        unregisterGlobalCiDiagnostics(this);
    }
    registered_owner_ = false;
    if (registered_owner && mapping_owner && region_ != nullptr) {
        region_->ready.store(0, std::memory_order_seq_cst);
    }
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (mapping_view_ != nullptr) {
        ::UnmapViewOfFile(mapping_view_);
    }
    if (mapping_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(mapping_handle_));
    }
    mapping_view_ = nullptr;
    mapping_handle_ = nullptr;
#endif
    mapping_owner_ = false;
    region_ = &local_region_;
    enabled_ = false;
    profiler_generation_.store(0, std::memory_order_relaxed);
    local_region_.ready.store(0, std::memory_order_seq_cst);
    mapping_name_.clear();
}

void CiDiagnostics::initializeRegion(CiDiagnosticsRegion &region, std::uint64_t mapping_lifetime_generation) noexcept
{
    region.ready.store(0, std::memory_order_seq_cst);
    region.magic.store(0, std::memory_order_seq_cst);
    region.version.store(0, std::memory_order_seq_cst);
    region.size.store(0, std::memory_order_seq_cst);
    region.generation.store(mapping_lifetime_generation, std::memory_order_seq_cst);
    for (auto &entry : region.records) {
        clearRecord(entry);
    }
    region.magic.store(kCiDiagnosticsMagic, std::memory_order_seq_cst);
    region.version.store(kCiDiagnosticsVersion, std::memory_order_seq_cst);
    region.size.store(sizeof(CiDiagnosticsRegion), std::memory_order_seq_cst);
    region.ready.store(kCiDiagnosticsReady, std::memory_order_seq_cst);
}

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
bool CiDiagnostics::openMapping() noexcept
{
    HANDLE handle = nullptr;
    void *view = nullptr;
    CiDiagnosticsRegion *mapped = nullptr;
    bool ready_published = false;
    try {
        const auto pid = static_cast<std::uint64_t>(::GetCurrentProcessId());
        mapping_name_ = mappingNameForPid(pid);
        const std::wstring wide_name(mapping_name_.begin(), mapping_name_.end());
        static_assert(sizeof(CiDiagnosticsRegion) <= std::numeric_limits<DWORD>::max());
        ::SetLastError(ERROR_SUCCESS);
        handle = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(sizeof(CiDiagnosticsRegion)), wide_name.c_str());
        if (handle == nullptr) {
            mapping_name_.clear();
            return false;
        }
        const bool fresh = ::GetLastError() != ERROR_ALREADY_EXISTS;
        view = ::MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CiDiagnosticsRegion));
        if (view == nullptr) {
            ::CloseHandle(handle);
            handle = nullptr;
            mapping_name_.clear();
            return false;
        }

        mapped = static_cast<CiDiagnosticsRegion *>(view);
        if (fresh) {
            std::construct_at(mapped);
            initializeRegion(*mapped, 1);
        }
        else {
            const bool valid_header = mapped->magic.load(std::memory_order_seq_cst) == kCiDiagnosticsMagic &&
                                      mapped->version.load(std::memory_order_seq_cst) == kCiDiagnosticsVersion &&
                                      mapped->size.load(std::memory_order_seq_cst) == sizeof(CiDiagnosticsRegion);
            const std::uint64_t ready = mapped->ready.load(std::memory_order_seq_cst);
            if (!valid_header || ready != 0) {
                ::UnmapViewOfFile(view);
                ::CloseHandle(handle);
                view = nullptr;
                handle = nullptr;
                mapping_name_.clear();
                return false;
            }
            const std::uint64_t prior_generation = mapped->generation.load(std::memory_order_seq_cst);
            const std::uint64_t next_generation =
                prior_generation == std::numeric_limits<std::uint64_t>::max() ? 1 : prior_generation + 1;
            for (auto &entry : mapped->records) {
                invalidateRecord(entry);
            }
            mapped->generation.store(next_generation, std::memory_order_seq_cst);
            mapped->ready.store(kCiDiagnosticsReady, std::memory_order_seq_cst);
        }
        ready_published = true;
        mapping_handle_ = handle;
        mapping_view_ = view;
        region_ = mapped;
        enabled_ = true;
        mapping_owner_ = true;
        handle = nullptr;
        view = nullptr;
        return true;
    }
    catch (...) {
        if (ready_published && mapped != nullptr) {
            mapped->ready.store(0, std::memory_order_seq_cst);
        }
        if (view != nullptr) {
            ::UnmapViewOfFile(view);
        }
        if (handle != nullptr) {
            ::CloseHandle(handle);
        }
        mapping_owner_ = false;
        mapping_name_.clear();
        region_ = &local_region_;
        enabled_ = false;
        return false;
    }
}
#endif

std::uint64_t CiDiagnostics::beginGeneration() noexcept
{
    if (!enabled_ || region_ == nullptr) {
        return 0;
    }
    return profiler_generation_.fetch_add(1, std::memory_order_seq_cst) + 1;
}

std::uint64_t CiDiagnostics::generation() const noexcept
{
    return enabled_ && region_ != nullptr ? profiler_generation_.load(std::memory_order_seq_cst) : 0;
}

CiDiagnosticRecord *CiDiagnostics::record(CiDiagnosticContext context) noexcept
{
    const auto index = static_cast<std::size_t>(context);
    if (index >= kCiDiagnosticsRecordCount || region_ == nullptr) {
        return nullptr;
    }
    return &region_->records[index];
}

void CiDiagnostics::publish(CiDiagnosticContext context, CiDiagnosticPhase phase_value, std::uint64_t worker_tid,
                            std::uint64_t target_tid, CiDiagnosticCounter counter, std::uint64_t counter_delta) noexcept
{
    if (!enabled_) {
        return;
    }
    CiDiagnosticRecord *entry = record(context);
    if (entry == nullptr) {
        return;
    }

    const std::uint64_t current = entry->sequence.load(std::memory_order_seq_cst);
    const std::uint64_t odd = (current & ~1ULL) + 1;
    entry->sequence.store(odd, std::memory_order_seq_cst);
    entry->generation.store(profiler_generation_.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
    if (worker_tid != 0) {
        entry->worker_tid.store(worker_tid, std::memory_order_seq_cst);
    }
    if (target_tid != 0) {
        entry->target_tid.store(target_tid, std::memory_order_seq_cst);
    }
    const std::uint64_t transition = nextTransition(*entry);
    entry->phase_and_transition.store(
        (transition << ci_diagnostics_snapshot_detail::kTransitionShift) |
            (static_cast<std::uint64_t>(phase_value) & ci_diagnostics_snapshot_detail::kPhaseMask),
        std::memory_order_seq_cst);
    if (counter_delta != 0) {
        switch (counter) {
        case CiDiagnosticCounter::SuspendSuccess:
            entry->suspend_success_count.fetch_add(counter_delta, std::memory_order_seq_cst);
            break;
        case CiDiagnosticCounter::ResumeSuccess:
            entry->resume_success_count.fetch_add(counter_delta, std::memory_order_seq_cst);
            break;
        case CiDiagnosticCounter::WalkCalls:
            entry->walk_call_count.fetch_add(counter_delta, std::memory_order_seq_cst);
            break;
        case CiDiagnosticCounter::None:
            break;
        }
    }
    entry->sequence.store(odd + 1, std::memory_order_seq_cst);
}

CiDiagnostics *globalCiDiagnostics() noexcept
{
    return GDiagnostics.load(std::memory_order_seq_cst);
}

bool registerGlobalCiDiagnostics(CiDiagnostics *diagnostics) noexcept
{
    if (diagnostics == nullptr) {
        return false;
    }
    CiDiagnostics *expected = nullptr;
    return GDiagnostics.compare_exchange_strong(expected, diagnostics, std::memory_order_seq_cst,
                                                std::memory_order_seq_cst);
}

void unregisterGlobalCiDiagnostics(CiDiagnostics *diagnostics) noexcept
{
    if (diagnostics == nullptr) {
        return;
    }
    CiDiagnostics *expected = diagnostics;
    GDiagnostics.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst, std::memory_order_seq_cst);
}

std::uint64_t ciDiagnosticCurrentThreadId() noexcept
{
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    return 0;
#endif
}

CiDiagnosticContext samplerLifecycleDiagnosticContext() noexcept
{
    return GLiveExportScope ? CiDiagnosticContext::SamplerLifecycleLiveExport
                            : CiDiagnosticContext::SamplerLifecycleMain;
}

CiDiagnostics::Scope::Scope(CiDiagnostics *diagnostics, CiDiagnosticContext context, CiDiagnosticPhase entry,
                            CiDiagnosticPhase normal_exit, CiDiagnosticPhase exceptional_exit, std::uint64_t worker_tid,
                            std::uint64_t target_tid) noexcept
    : diagnostics_(diagnostics), context_(context), normal_exit_(normal_exit), exceptional_exit_(exceptional_exit),
      worker_tid_(worker_tid), target_tid_(target_tid), uncaught_on_entry_(std::uncaught_exceptions())
{
    if (diagnostics_ != nullptr) {
        diagnostics_->publish(context_, entry, worker_tid_, target_tid_);
    }
}

CiDiagnostics::Scope::~Scope() noexcept
{
    if (diagnostics_ != nullptr) {
        diagnostics_->publish(context_,
                              std::uncaught_exceptions() > uncaught_on_entry_ ? exceptional_exit_ : normal_exit_,
                              worker_tid_, target_tid_);
    }
}

CiDiagnostics::LiveExportScope::LiveExportScope() noexcept : previous_(GLiveExportScope)
{
    GLiveExportScope = true;
}

CiDiagnostics::LiveExportScope::~LiveExportScope() noexcept
{
    GLiveExportScope = previous_;
}

}  // namespace spark
