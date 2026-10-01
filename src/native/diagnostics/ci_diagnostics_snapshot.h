#ifndef ENDSTONE_SPARK_CI_DIAGNOSTICS_SNAPSHOT_H
#define ENDSTONE_SPARK_CI_DIAGNOSTICS_SNAPSHOT_H

#include <cstdint>
#include <type_traits>

#include "native/diagnostics/ci_diagnostics.h"

namespace spark::ci_diagnostics_snapshot_detail {

inline constexpr std::uint64_t kPhaseMask = 0xffffULL;
inline constexpr std::uint64_t kTransitionShift = 16;
inline constexpr std::uint64_t kTransitionMask = (1ULL << 48) - 1;

struct NoSequenceReadHook final {
    void operator()() const noexcept {}
};

template <typename SequenceReadHook>
CiDiagnosticSnapshot readSnapshot(const CiDiagnosticRecord &record,
                                  SequenceReadHook after_before_sequence_load) noexcept
{
    CiDiagnosticSnapshot snapshot;
    const std::uint64_t before = record.sequence.load(std::memory_order_seq_cst);
    if ((before & 1U) != 0) {
        return snapshot;
    }

    if constexpr (!std::is_same_v<SequenceReadHook, NoSequenceReadHook>) {
        after_before_sequence_load();
    }

    snapshot.sequence = before;
    snapshot.generation = record.generation.load(std::memory_order_seq_cst);
    const std::uint64_t packed = record.phase_and_transition.load(std::memory_order_seq_cst);
    snapshot.phase = static_cast<CiDiagnosticPhase>(packed & kPhaseMask);
    snapshot.transition = (packed >> kTransitionShift) & kTransitionMask;
    snapshot.worker_tid = record.worker_tid.load(std::memory_order_seq_cst);
    snapshot.target_tid = record.target_tid.load(std::memory_order_seq_cst);
    snapshot.suspend_success_count = record.suspend_success_count.load(std::memory_order_seq_cst);
    snapshot.resume_success_count = record.resume_success_count.load(std::memory_order_seq_cst);
    snapshot.walk_call_count = record.walk_call_count.load(std::memory_order_seq_cst);

    const std::uint64_t after = record.sequence.load(std::memory_order_seq_cst);
    if (before != after || (after & 1U) != 0) {
        return CiDiagnosticSnapshot{};
    }
    snapshot.sequence = after;
    snapshot.consistent = true;
    return snapshot;
}

}  // namespace spark::ci_diagnostics_snapshot_detail

#endif  // ENDSTONE_SPARK_CI_DIAGNOSTICS_SNAPSHOT_H
