#include "application/activity/activity_command.h"

#include <chrono>
#include <string>
#include <vector>

#include "core/util/format.h"

namespace spark {

namespace {

std::string formatDateDiff(std::int64_t time_ms, std::int64_t now_ms)
{
    std::int64_t seconds = (now_ms - time_ms) / 1000;
    if (seconds <= 0) {
        return "now";
    }

    std::int64_t minute = seconds / 60;
    seconds = seconds % 60;
    std::int64_t hour = minute / 60;
    minute = minute % 60;
    std::int64_t day = hour / 24;
    hour = hour % 24;

    std::string result;
    if (day != 0) {
        result += std::to_string(day) + "d ";
    }
    if (hour != 0) {
        result += std::to_string(hour) + "h ";
    }
    if (minute != 0) {
        result += std::to_string(minute) + "m ";
    }
    if (seconds != 0) {
        result += std::to_string(seconds) + "s";
    }

    if (result.empty()) {
        result = "0s";
    }
    if (result.back() == ' ') {
        result.pop_back();
    }
    return result + " ago";
}

}  // namespace

void ActivityCommand::cmdActivity(CommandSender &sender, const Arguments &args)
{
    auto entries = log_.entries();
    if (entries.empty()) {
        sender.sendMessage("{}There are no entries present in the log.{}", kColorGold, kColorGray);
        return;
    }

    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    // Simple pagination: 4 entries per page.
    constexpr int kPerPage = 4;
    int page = 1;
    auto page_flag = args.intFlag("page");
    if (args.boolFlag("page") && page_flag && *page_flag >= 1) {
        page = static_cast<int>(*page_flag);
    }

    int total = static_cast<int>(entries.size());
    int pages = (total + kPerPage - 1) / kPerPage;
    if (page > pages) {
        sender.sendMessage("{}Unknown page selected. {} total pages.{}", kColorGold, pages, kColorGray);
        return;
    }

    int start = (page - 1) * kPerPage;
    int end = std::min(start + kPerPage, total);

    sender.sendMessage("{}Recent spark activity {}(page {}/{}){}:", kColorGold, kColorGray, page, pages, kColorReset);

    for (int i = start; i < end; ++i) {
        const Activity &a = entries[i];
        sender.sendMessage("{}> {}#{} {}- {} {}{}", kColorDarkGray, kColorWhite, i + 1, kColorDarkGray, kColorYellow,
                           a.type, kColorReset);
        sender.sendMessage("  {}Created by: {}{}", kColorGray, kColorReset, a.user_name);
        const char *label = a.data_type == Activity::DataType::Url ? "URL" : "File";
        sender.sendMessage("  {}{}: {}{}", kColorGray, label, kColorReset, a.data_value);
        sender.sendMessage("  {}{}{}", kColorGray, formatDateDiff(a.time_ms, now_ms), kColorReset);
    }
}

}  // namespace spark
