#ifndef ENDSTONE_SPARK_ACTIVITY_LOG_H
#define ENDSTONE_SPARK_ACTIVITY_LOG_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace spark {

struct Activity {
    std::string user_name;
    bool user_is_player = false;
    std::int64_t time_ms = 0;
    std::string type;

    enum class DataType {
        Url,
        File
    };
    DataType data_type = DataType::Url;
    std::string data_value;

    static Activity url(std::string user_name, bool user_is_player, std::int64_t time_ms, std::string type,
                        std::string url);
    static Activity file(std::string user_name, bool user_is_player, std::int64_t time_ms, std::string type,
                         std::string path);

    bool shouldExpire(std::int64_t now_ms) const;
    std::string serialize() const;
    static bool deserialize(const std::string &json, Activity &out);
};

class ActivityLog {
public:
    static constexpr std::size_t kMaxEntries = 100;

    explicit ActivityLog(std::filesystem::path file);

    void add(const Activity &activity);
    std::vector<Activity> entries() const;

    void load();
    void save() const;

private:
    std::filesystem::path file_;
    std::vector<Activity> entries_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_ACTIVITY_LOG_H
