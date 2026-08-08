#ifndef ENDSTONE_SPARK_NETWORK_MONITOR_H
#define ENDSTONE_SPARK_NETWORK_MONITOR_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace spark {

struct NetworkInterfaceInfo {
    std::string name;
    std::int64_t rx_bytes = 0;
    std::int64_t rx_packets = 0;
    std::int64_t rx_errors = 0;
    std::int64_t tx_bytes = 0;
    std::int64_t tx_packets = 0;
    std::int64_t tx_errors = 0;

    bool isZero() const;
    NetworkInterfaceInfo subtract(const NetworkInterfaceInfo &other) const;
};

// Bounded rolling average for double values, matching upstream spark's
// RollingAverage: mean/max/min/median/p95 over a fixed window.
class DoubleRollingAverage {
public:
    explicit DoubleRollingAverage(std::size_t window_size);
    void add(double value);
    std::size_t samples() const { return count_; }
    double mean() const;
    double max() const;
    double min() const;
    double median() const;
    double percentile95() const;

private:
    std::vector<double> sortedCopy() const;
    double percentile(double p) const;
    std::size_t capacity_;
    std::vector<double> samples_;
    std::size_t count_ = 0;
    std::size_t head_ = 0;
    double total_ = 0.0;
};

// Snapshot of a rolling average's computed values, matching proto RollingAverageValues.
struct NetworkRateValues {
    bool present = false;
    double mean = 0.0;
    double max = 0.0;
    double min = 0.0;
    double median = 0.0;
    double percentile95 = 0.0;
};

struct NetworkInterfaceAverages {
    explicit NetworkInterfaceAverages(std::size_t window_size);
    void accept(const NetworkInterfaceInfo &info, double poll_interval_seconds);
    DoubleRollingAverage rx_bytes_per_second;
    DoubleRollingAverage tx_bytes_per_second;
    DoubleRollingAverage rx_packets_per_second;
    DoubleRollingAverage tx_packets_per_second;
};

struct NetworkInterfaceSnapshot {
    NetworkRateValues rx_bytes_per_second;
    NetworkRateValues tx_bytes_per_second;
    NetworkRateValues rx_packets_per_second;
    NetworkRateValues tx_packets_per_second;
};

// Polls network interface statistics at a fixed interval and maintains
// per-interface rolling averages of RX/TX bytes and packets per second.
class NetworkMonitor {
public:
    using PollFn = std::function<std::map<std::string, NetworkInterfaceInfo>()>;

    static constexpr int kPollIntervalSeconds = 60;
    static constexpr int kWindowSizeSeconds = 15 * 60;
    static constexpr int kWindowSize = kWindowSizeSeconds / kPollIntervalSeconds;  // 15

    NetworkMonitor();
    explicit NetworkMonitor(PollFn poll_fn);

    // Polls the system and updates rolling averages. Returns false on the first
    // call (no previous reading to diff against), true once averages are available.
    bool poll();

    std::map<std::string, NetworkInterfaceSnapshot> snapshot() const;
    std::map<std::string, NetworkInterfaceInfo> systemTotals() const;

private:
    static bool shouldIgnore(const std::string &name);

    PollFn poll_fn_;
    std::map<std::string, NetworkInterfaceInfo> previous_;
    std::map<std::string, NetworkInterfaceAverages> averages_;
    bool first_poll_ = true;
};

// Platform-specific: reads all network interface counters.
// Returns an empty map if the platform does not support network monitoring.
std::map<std::string, NetworkInterfaceInfo> pollNetworkInterfaces();

}  // namespace spark

#endif  // ENDSTONE_SPARK_NETWORK_MONITOR_H
