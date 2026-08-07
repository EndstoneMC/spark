#include "core/command/arguments.h"

#include <charconv>
#include <cmath>
#include <utility>

namespace spark {

namespace {

bool isFlag(const std::string &token)
{
    return token.rfind("--", 0) == 0;
}

}  // namespace

std::vector<std::string> Arguments::tokenize(const std::string &line)
{
    std::vector<std::string> tokens;
    std::string token;
    char quote = '\0';
    for (char ch : line) {
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            }
            else {
                token.push_back(ch);
            }
        }
        else if (ch == '\'' || ch == '"') {
            quote = ch;
        }
        else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            if (!token.empty()) {
                tokens.push_back(std::move(token));
                token.clear();
            }
        }
        else {
            token.push_back(ch);
        }
    }
    if (!token.empty()) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

Arguments::Arguments(const std::vector<std::string> &tokens) : raw_(tokens)
{
    std::size_t i = 0;
    if (!raw_.empty() && !isFlag(raw_[0])) {
        sub_ = raw_[0];
        i = 1;
    }
    for (; i < raw_.size(); ++i) {
        if (!isFlag(raw_[i])) {
            continue;
        }
        std::string name = raw_[i].substr(2);
        present_.insert(name);
        if (i + 1 < raw_.size() && !isFlag(raw_[i + 1])) {
            values_.emplace(name, raw_[i + 1]);
            ++i;
        }
    }
}

bool Arguments::boolFlag(const std::string &name) const
{
    return present_.count(name) > 0;
}

std::optional<long> Arguments::intFlag(const std::string &name) const
{
    auto it = values_.find(name);
    if (it == values_.end()) {
        return std::nullopt;
    }
    const std::string &text = it->second;
    long value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> Arguments::doubleFlag(const std::string &name) const
{
    auto it = values_.find(name);
    if (it == values_.end()) {
        return std::nullopt;
    }
    const std::string &text = it->second;
    double value = 0.0;
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string> Arguments::stringFlag(const std::string &name) const
{
    std::vector<std::string> out;
    auto range = values_.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) {
        out.push_back(it->second);
    }
    return out;
}

}  // namespace spark
