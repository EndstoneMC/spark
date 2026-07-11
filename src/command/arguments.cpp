#include "command/arguments.h"

#include <cstdlib>
#include <sstream>

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
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
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

long Arguments::intFlag(const std::string &name, long fallback) const
{
    auto it = values_.find(name);
    if (it == values_.end()) {
        return fallback;
    }
    return std::strtol(it->second.c_str(), nullptr, 10);
}

double Arguments::doubleFlag(const std::string &name, double fallback) const
{
    auto it = values_.find(name);
    if (it == values_.end()) {
        return fallback;
    }
    return std::strtod(it->second.c_str(), nullptr);
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
