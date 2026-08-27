#include "json.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

namespace diskscan {
namespace {

/// One code point, appended as UTF-8.
void appendUtf8(std::string* out, unsigned code) {
    if (code < 0x80) {
        out->push_back(char(code));
    } else if (code < 0x800) {
        out->push_back(char(0xC0 | (code >> 6)));
        out->push_back(char(0x80 | (code & 0x3F)));
    } else {
        out->push_back(char(0xE0 | (code >> 12)));
        out->push_back(char(0x80 | ((code >> 6) & 0x3F)));
        out->push_back(char(0x80 | (code & 0x3F)));
    }
}

// ---- A very small JSON reader -------------------------------------------
//
// Hand-written rather than pulled in, because the whole need is four fields out
// of one known response. It is a real parser rather than a search for quoted
// keys: a release description containing a brace or a quote would defeat the
// latter, and that is exactly the kind of bug that only shows up in production.

class JsonParser {
public:
    explicit JsonParser(const std::string& source) : s_(source) {}

    std::shared_ptr<JsonValue> parse() {
        auto value = parseValue();
        return failed_ ? nullptr : value;
    }

private:
    void skipSpace() {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' ||
                                   s_[at_] == '\r')) {
            ++at_;
        }
    }

    bool consume(char c) {
        skipSpace();
        if (at_ < s_.size() && s_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }

    bool literal(const char* word) {
        size_t length = strlen(word);
        if (s_.compare(at_, length, word) != 0) return false;
        at_ += length;
        return true;
    }

    std::string parseString() {
        std::string out;
        if (!consume('"')) {
            failed_ = true;
            return out;
        }
        while (at_ < s_.size()) {
            char c = s_[at_++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_ >= s_.size()) break;
            char escape = s_[at_++];
            switch (escape) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (at_ + 4 > s_.size()) {
                        failed_ = true;
                        return out;
                    }
                    unsigned code = std::strtoul(s_.substr(at_, 4).c_str(), nullptr, 16);
                    at_ += 4;
                    // Only the fields actually read matter, and those are ASCII;
                    // anything above it is kept as UTF-8 rather than dropped. A
                    // surrogate pair is not reassembled - no field this reads can
                    // contain one, and encoding each half separately at least
                    // keeps the string the length it should be.
                    appendUtf8(&out, code);
                    break;
                }
                default: out.push_back(escape); break;
            }
        }
        failed_ = true;
        return out;
    }

    std::shared_ptr<JsonValue> parseValue() {
        if (++depth_ > 32) {
            failed_ = true;
            return nullptr;
        }
        auto value = std::make_shared<JsonValue>();
        skipSpace();
        if (at_ >= s_.size()) {
            failed_ = true;
            --depth_;
            return value;
        }
        char c = s_[at_];
        if (c == '{') {
            ++at_;
            value->kind = JsonValue::Kind::Object;
            skipSpace();
            if (!consume('}')) {
                do {
                    skipSpace();
                    std::string key = parseString();
                    if (failed_ || !consume(':')) {
                        failed_ = true;
                        break;
                    }
                    value->object[key] = parseValue();
                } while (consume(','));
                if (!consume('}')) failed_ = true;
            }
        } else if (c == '[') {
            ++at_;
            value->kind = JsonValue::Kind::Array;
            skipSpace();
            if (!consume(']')) {
                do {
                    value->array.push_back(parseValue());
                } while (consume(','));
                if (!consume(']')) failed_ = true;
            }
        } else if (c == '"') {
            value->kind = JsonValue::Kind::String;
            value->text = parseString();
        } else if (literal("true")) {
            value->kind = JsonValue::Kind::Bool;
            value->boolean = true;
        } else if (literal("false")) {
            value->kind = JsonValue::Kind::Bool;
            value->boolean = false;
        } else if (literal("null")) {
            value->kind = JsonValue::Kind::Null;
        } else {
            char* end = nullptr;
            value->number = std::strtod(s_.c_str() + at_, &end);
            if (!end || end == s_.c_str() + at_) {
                failed_ = true;
            } else {
                value->kind = JsonValue::Kind::Number;
                at_ = size_t(end - s_.c_str());
            }
        }
        --depth_;
        return value;
    }

    const std::string& s_;
    size_t at_ = 0;
    int depth_ = 0;
    bool failed_ = false;
};

}  // namespace

std::shared_ptr<JsonValue> parseJson(const std::string& source) {
    JsonParser parser(source);
    return parser.parse();
}

const JsonValue* member(const JsonValue* value, const char* key) {
    if (!value || value->kind != JsonValue::Kind::Object) return nullptr;
    auto it = value->object.find(key);
    return it == value->object.end() ? nullptr : it->second.get();
}

std::string stringMember(const JsonValue* value, const char* key) {
    const JsonValue* found = member(value, key);
    return (found && found->kind == JsonValue::Kind::String) ? found->text : std::string();
}

bool isNewerVersion(const std::string& candidate, const std::string& current) {
    auto split = [](const std::string& text) {
        std::vector<int> parts;
        size_t at = 0;
        while (at <= text.size()) {
            size_t dot = text.find('.', at);
            if (dot == std::string::npos) dot = text.size();
            parts.push_back(std::atoi(text.substr(at, dot - at).c_str()));
            at = dot + 1;
        }
        return parts;
    };
    const std::vector<int> a = split(candidate), b = split(current);
    const size_t count = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < count; ++i) {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

int daysSince(const std::string& date) {
    if (date.size() < 10) return -1;
    std::tm when{};
    when.tm_year = std::atoi(date.substr(0, 4).c_str()) - 1900;
    when.tm_mon = std::atoi(date.substr(5, 2).c_str()) - 1;
    when.tm_mday = std::atoi(date.substr(8, 2).c_str());
    when.tm_hour = 12;  // midday, so a time-zone slip cannot move the day
#ifdef _WIN32
    const std::time_t then = _mkgmtime(&when);
#else
    const std::time_t then = timegm(&when);
#endif
    if (then <= 0) return -1;
    const double seconds = std::difftime(std::time(nullptr), then);
    return seconds <= 0 ? -1 : int(seconds / 86400.0);
}

}  // namespace diskscan
