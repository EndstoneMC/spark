#include "core/config/trusted_viewers.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace spark {

namespace {

std::string jsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char ch : s) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            }
            else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

// Minimal JSON string-array parser: accepts ["...", "...", ...] with optional whitespace.
bool parseStringArray(const std::string &text, std::vector<std::string> &out)
{
    std::size_t pos = 0;
    auto skip_ws = [&]() {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
            ++pos;
        }
    };
    skip_ws();
    if (pos >= text.size() || text[pos] != '[') {
        return false;
    }
    ++pos;
    skip_ws();
    if (pos < text.size() && text[pos] == ']') {
        return true;
    }
    while (pos < text.size()) {
        skip_ws();
        if (pos >= text.size() || text[pos] != '"') {
            return false;
        }
        ++pos;
        std::string str;
        while (pos < text.size()) {
            char ch = text[pos++];
            if (ch == '"') {
                break;
            }
            if (ch == '\\' && pos < text.size()) {
                char esc = text[pos++];
                switch (esc) {
                case '"':
                    str += '"';
                    break;
                case '\\':
                    str += '\\';
                    break;
                case '/':
                    str += '/';
                    break;
                case 'b':
                    str += '\b';
                    break;
                case 'f':
                    str += '\f';
                    break;
                case 'n':
                    str += '\n';
                    break;
                case 'r':
                    str += '\r';
                    break;
                case 't':
                    str += '\t';
                    break;
                case 'u':
                    if (pos + 4 > text.size()) {
                        return false;
                    }
                    str += '?';
                    pos += 4;
                    break;
                default:
                    return false;
                }
            }
            else {
                str += ch;
            }
        }
        out.push_back(std::move(str));
        skip_ws();
        if (pos >= text.size()) {
            return false;
        }
        if (text[pos] == ',') {
            ++pos;
            continue;
        }
        if (text[pos] == ']') {
            ++pos;
            return true;
        }
        return false;
    }
    return false;
}

}  // namespace

TrustedViewersState::TrustedViewersState(std::filesystem::path file) : file_(std::move(file)) {}

bool TrustedViewersState::load()
{
    last_error_.clear();
    keys_.clear();

    if (!std::filesystem::exists(file_)) {
        return false;
    }

    std::ifstream in(file_);
    if (!in) {
        last_error_ = "Unable to open trusted-viewers file for reading";
        return false;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    if (!parseStringArray(text, keys_)) {
        last_error_ = "Malformed JSON in trusted-viewers file";
        keys_.clear();
        return false;
    }
    return true;
}

bool TrustedViewersState::save() const
{
    last_error_.clear();

    std::ostringstream ss;
    ss << "[\n";
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        ss << "  \"" << jsonEscape(keys_[i]) << "\"";
        if (i + 1 < keys_.size()) {
            ss << ",";
        }
        ss << "\n";
    }
    ss << "]\n";

    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);

    std::filesystem::path tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) {
            last_error_ = "Unable to open trusted-viewers file for writing";
            return false;
        }
        out << ss.str();
        out.close();
    }
    std::filesystem::rename(tmp, file_, ec);
    if (ec) {
        last_error_ = "Unable to rename trusted-viewers file: " + ec.message();
        return false;
    }
    return true;
}

bool TrustedViewersState::contains(const std::string &b64_key) const
{
    return std::ranges::find(keys_, b64_key) != keys_.end();
}

void TrustedViewersState::add(const std::string &b64_key)
{
    if (!contains(b64_key)) {
        keys_.push_back(b64_key);
    }
}

}  // namespace spark
