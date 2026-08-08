#ifndef SPARK_CORE_STATS_PING_STATISTICS_H
#define SPARK_CORE_STATS_PING_STATISTICS_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace spark {

// Sorted-array ping summary with min/median/p95/max, matching upstream
// PingSummary.java's percentile computation: index = ceil(p * (n-1)).
struct PingSummary {
    PingSummary() = default;
    explicit PingSummary(std::vector<int> values);

    int total() const { return static_cast<int>(sorted_.size()); }
    int min() const;
    int median() const;
    int percentile95th() const;
    int max() const;

private:
    int percentile(double p) const;
    std::vector<int> sorted_;
};

// Rolling average of median ping over a fixed-capacity window.
// Each add() contributes one median value; query-time statistics are
// computed from the accumulated samples.
struct PingRollingAverage {
    explicit PingRollingAverage(std::size_t window_size)
        : capacity_(window_size)
    {
        samples_.reserve(window_size);
    }

    void add(int value);
    std::size_t samples() const { return count_; }
    const std::vector<int> &rawSamples() const { return samples_; }

    double mean() const;
    int min() const;
    int median() const;
    int percentile95th() const;
    int max() const;

private:
    std::vector<int> sortedCopy() const;

    std::size_t capacity_;
    std::vector<int> samples_;
    std::size_t count_ = 0;
    std::size_t head_ = 0;
};

// A single player's ping snapshot.
struct PlayerPing {
    std::string name;
    int ping = 0;

    bool found() const { return !name.empty(); }
};

// Polls player ping values in milliseconds. Implemented by the platform adapter.
class PlayerPingProvider {
public:
    virtual ~PlayerPingProvider() = default;
    // Returns a map of player name -> ping in milliseconds.
    virtual std::map<std::string, int> poll() = 0;
};

// Collects player ping statistics on a fixed interval and maintains a rolling
// 15-minute average of the median ping across all players.
class PingStatistics {
public:
    static constexpr int kQueryRateSeconds = 10;
    static constexpr int kWindowSizeSeconds = 15 * 60;  // 900
    static constexpr int kWindowSize = kWindowSizeSeconds / kQueryRateSeconds;  // 90

    explicit PingStatistics(PlayerPingProvider &provider);

    // Polls current pings and feeds the rolling average. Returns true if
    // the rolling average was updated (i.e. there were players with ping > 0).
    bool poll();

    // Current snapshot of all player pings.
    PingSummary currentSummary() const;

    // Queries a specific player's ping (case-insensitive).
    PlayerPing query(const std::string &player_name) const;

    const PingRollingAverage &rollingAverage() const { return rolling_average_; }

private:
    PlayerPingProvider &provider_;
    PingRollingAverage rolling_average_;
};

}  // namespace spark

#endif  // SPARK_CORE_STATS_PING_STATISTICS_H
