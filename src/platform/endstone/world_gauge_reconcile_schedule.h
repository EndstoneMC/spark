#ifndef SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_RECONCILE_SCHEDULE_H
#define SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_RECONCILE_SCHEDULE_H

#include <cstdint>

namespace spark::endstone_adapter {

class WorldGaugeReconcileSchedule {
public:
    void start(std::int64_t now_ms) noexcept
    {
        started_ = true;
        started_at_ms_ = now_ms;
        last_reconcile_ms_ = now_ms;
        has_tile_attempt_ = false;
        last_tile_attempt_started_ms_ = 0;
        last_tile_attempt_completed_at_ms_ = 0;
        last_tile_attempt_complete_ = false;
        has_tile_success_ = false;
        last_tile_success_ms_ = 0;
    }

    [[nodiscard]] bool entityReconcileDue(std::int64_t now_ms) const noexcept
    {
        return started_ && elapsed(now_ms, last_reconcile_ms_, 30000);
    }

    [[nodiscard]] bool tileReconcileDue(std::int64_t now_ms, bool tile_scan_supported) const noexcept
    {
        if (!started_) {
            return false;
        }
        if (!has_tile_attempt_) {
            return elapsed(now_ms, started_at_ms_, 60000);
        }
        if (now_ms < last_tile_attempt_started_ms_) {
            return false;
        }
        if (!tile_scan_supported) {
            return elapsed(now_ms, last_tile_attempt_completed_at_ms_, 60000);
        }
        if (!last_tile_attempt_complete_) {
            return elapsed(now_ms, last_tile_attempt_completed_at_ms_, 5000);
        }
        return has_tile_success_ && elapsed(now_ms, last_tile_success_ms_, 60000);
    }

    void recordReconcile(std::int64_t now_ms) noexcept { last_reconcile_ms_ = now_ms; }

    template <typename Now, typename Scan, typename FailureNow>
    void runReconcile(Now &&now, Scan &&scan, bool include_tile_entities, FailureNow &&failure_now)
    {
        static_assert(noexcept(failure_now()));
        std::int64_t attempt_ms = failure_now();
        try {
            attempt_ms = now();
            const bool tile_scan_complete = scan();
            const std::int64_t completed_at_ms = now();
            recordReconcile(completed_at_ms);
            if (include_tile_entities) {
                recordTileAttempt(attempt_ms, tile_scan_complete, completed_at_ms);
            }
        }
        catch (...) {
            const std::int64_t completed_at_ms = failure_now();
            recordReconcile(completed_at_ms);
            if (include_tile_entities) {
                recordTileAttempt(attempt_ms, false, completed_at_ms);
            }
            throw;
        }
    }

    void recordTileAttempt(std::int64_t attempt_ms, bool complete, std::int64_t completed_at_ms) noexcept
    {
        has_tile_attempt_ = true;
        last_tile_attempt_started_ms_ = attempt_ms;
        last_tile_attempt_completed_at_ms_ = completed_at_ms;
        last_tile_attempt_complete_ = complete;
        if (complete) {
            has_tile_success_ = true;
            last_tile_success_ms_ = completed_at_ms;
        }
    }

private:
    static bool elapsed(std::int64_t now_ms, std::int64_t since_ms, std::int64_t interval_ms) noexcept
    {
        return now_ms >= since_ms && now_ms - since_ms >= interval_ms;
    }

    std::int64_t started_at_ms_ = 0;
    std::int64_t last_reconcile_ms_ = 0;
    std::int64_t last_tile_attempt_started_ms_ = 0;
    std::int64_t last_tile_attempt_completed_at_ms_ = 0;
    std::int64_t last_tile_success_ms_ = 0;
    bool started_ = false;
    bool has_tile_attempt_ = false;
    bool last_tile_attempt_complete_ = false;
    bool has_tile_success_ = false;
};

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_RECONCILE_SCHEDULE_H
