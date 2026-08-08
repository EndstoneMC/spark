#include "core/config/spark_config.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace spark {

namespace {

// --- Minimal JSON value model (flat, sufficient for config.json) ---

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::vector<std::pair<std::string, JsonValue>> obj_val;

    const JsonValue *find(std::string_view key) const
    {
        for (const auto &[k, v] : obj_val) {
            if (k == key) return &v;
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string &text) : text_(text) {}

    bool parse(JsonValue &out)
    {
        skipWs();
        return parseValue(out);
    }

private:
    void skipWs()
    {
        while (pos_ < text_.size()) {
            char ch = text_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
                ++pos_;
            else
                break;
        }
    }

    bool parseValue(JsonValue &out)
    {
        skipWs();
        if (pos_ >= text_.size()) return false;
        char ch = text_[pos_];
        if (ch == '{') return parseObject(out);
        if (ch == '[') return parseArray(out);
        if (ch == '"') return parseString(out);
        if (ch == 't' || ch == 'f') return parseBool(out);
        if (ch == 'n') return parseNull(out);
        return parseNumber(out);
    }

    bool parseObject(JsonValue &out)
    {
        out.type = JsonValue::Type::Object;
        ++pos_;
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return true; }
        while (pos_ < text_.size()) {
            skipWs();
            if (pos_ >= text_.size() || text_[pos_] != '"') return false;
            JsonValue key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos_ >= text_.size() || text_[pos_] != ':') return false;
            ++pos_;
            JsonValue val;
            if (!parseValue(val)) return false;
            out.obj_val.emplace_back(key.str_val, std::move(val));
            skipWs();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == '}') { ++pos_; return true; }
            return false;
        }
        return false;
    }

    bool parseArray(JsonValue &out)
    {
        out.type = JsonValue::Type::Array;
        ++pos_;
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }
        while (pos_ < text_.size()) {
            JsonValue val;
            if (!parseValue(val)) return false;
            out.arr_val.push_back(std::move(val));
            skipWs();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == ']') { ++pos_; return true; }
            return false;
        }
        return false;
    }

    bool parseString(JsonValue &out)
    {
        out.type = JsonValue::Type::String;
        ++pos_;
        out.str_val.clear();
        while (pos_ < text_.size()) {
            char ch = text_[pos_++];
            if (ch == '"') return true;
            if (ch == '\\') {
                if (pos_ >= text_.size()) return false;
                char esc = text_[pos_++];
                switch (esc) {
                case '"':  out.str_val += '"'; break;
                case '\\': out.str_val += '\\'; break;
                case '/':  out.str_val += '/'; break;
                case 'b':  out.str_val += '\b'; break;
                case 'f':  out.str_val += '\f'; break;
                case 'n':  out.str_val += '\n'; break;
                case 'r':  out.str_val += '\r'; break;
                case 't':  out.str_val += '\t'; break;
                case 'u':
                    if (pos_ + 4 > text_.size()) return false;
                    out.str_val += '?';
                    pos_ += 4;
                    break;
                default: return false;
                }
            } else {
                out.str_val += ch;
            }
        }
        return false;
    }

    bool parseBool(JsonValue &out)
    {
        out.type = JsonValue::Type::Bool;
        if (text_.compare(pos_, 4, "true") == 0) { out.bool_val = true; pos_ += 4; return true; }
        if (text_.compare(pos_, 5, "false") == 0) { out.bool_val = false; pos_ += 5; return true; }
        return false;
    }

    bool parseNull(JsonValue &out)
    {
        out.type = JsonValue::Type::Null;
        if (text_.compare(pos_, 4, "null") == 0) { pos_ += 4; return true; }
        return false;
    }

    bool parseNumber(JsonValue &out)
    {
        out.type = JsonValue::Type::Number;
        std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool has_digit = false;
        while (pos_ < text_.size()) {
            char ch = text_[pos_];
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') {
                ++pos_;
                has_digit = true;
            } else {
                break;
            }
        }
        if (!has_digit) return false;
        try {
            out.num_val = std::stod(text_.substr(start, pos_ - start));
        } catch (...) {
            return false;
        }
        return true;
    }

    const std::string &text_;
    std::size_t pos_ = 0;
};

// --- JSON serialiser ---

std::string jsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char ch : s) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

}  // namespace

SparkConfig::SparkConfig(std::filesystem::path file)
    : file_(std::move(file))
{
}

bool SparkConfig::load()
{
    last_error_.clear();

    if (!std::filesystem::exists(file_)) {
        return false;
    }

    std::ifstream in(file_);
    if (!in) {
        last_error_ = "Unable to open config file for reading";
        return false;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    JsonParser parser(text);
    JsonValue root;
    if (!parser.parse(root) || root.type != JsonValue::Type::Object) {
        last_error_ = "Malformed JSON in config file - using defaults";
        return false;
    }

    // Strings
    if (const JsonValue *v = root.find("viewerUrl"); v && v->type == JsonValue::Type::String)
        viewer_url = v->str_val;
    if (const JsonValue *v = root.find("bytebinUrl"); v && v->type == JsonValue::Type::String)
        bytebin_url = v->str_val;
    if (const JsonValue *v = root.find("bytesocksHost"); v && v->type == JsonValue::Type::String)
        bytesocks_host = v->str_val;
    if (const JsonValue *v = root.find("backgroundProfilerThreadGrouper"); v && v->type == JsonValue::Type::String)
        background_profiler_thread_grouper = v->str_val;
    if (const JsonValue *v = root.find("backgroundProfilerThreadDumper"); v && v->type == JsonValue::Type::String)
        background_profiler_thread_dumper = v->str_val;

    // Booleans
    if (const JsonValue *v = root.find("backgroundProfiler"); v && v->type == JsonValue::Type::Bool)
        background_profiler_enabled = v->bool_val;
    if (const JsonValue *v = root.find("disableResponseBroadcast"); v && v->type == JsonValue::Type::Bool)
        disable_response_broadcast = v->bool_val;

    // Integers
    if (const JsonValue *v = root.find("backgroundProfilerInterval"); v && v->type == JsonValue::Type::Number)
        background_profiler_interval = static_cast<int>(v->num_val);

    // String array (trusted keys)
    if (const JsonValue *v = root.find("trustedKeys"); v && v->type == JsonValue::Type::Array) {
        trusted_keys.clear();
        for (const JsonValue &elem : v->arr_val) {
            if (elem.type == JsonValue::Type::String)
                trusted_keys.push_back(elem.str_val);
        }
    }

    return true;
}

bool SparkConfig::save() const
{
    last_error_.clear();

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"_header\": \"spark configuration file - https://spark.lucko.me/docs/Configuration\",\n";
    ss << "  \"viewerUrl\": \"" << jsonEscape(viewer_url) << "\",\n";
    ss << "  \"bytebinUrl\": \"" << jsonEscape(bytebin_url) << "\",\n";
    ss << "  \"bytesocksHost\": \"" << jsonEscape(bytesocks_host) << "\",\n";
    ss << "  \"backgroundProfiler\": " << (background_profiler_enabled ? "true" : "false") << ",\n";
    ss << "  \"backgroundProfilerInterval\": " << background_profiler_interval << ",\n";
    ss << "  \"backgroundProfilerThreadGrouper\": \"" << jsonEscape(background_profiler_thread_grouper) << "\",\n";
    ss << "  \"backgroundProfilerThreadDumper\": \"" << jsonEscape(background_profiler_thread_dumper) << "\",\n";
    ss << "  \"disableResponseBroadcast\": " << (disable_response_broadcast ? "true" : "false") << ",\n";
    ss << "  \"trustedKeys\": [";
    for (std::size_t i = 0; i < trusted_keys.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << "\"" << jsonEscape(trusted_keys[i]) << "\"";
    }
    ss << "]\n";
    ss << "}\n";

    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);

    std::filesystem::path tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) {
            last_error_ = "Unable to open config file for writing";
            return false;
        }
        out << ss.str();
        out.close();
    }
    std::filesystem::rename(tmp, file_, ec);
    if (ec) {
        last_error_ = "Unable to rename config file: " + ec.message();
        return false;
    }
    return true;
}

}  // namespace spark
