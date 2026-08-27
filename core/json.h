#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace diskscan {

/// A very small JSON reader, and the two comparisons the update check needs.
///
/// Hand-written rather than pulled in, because the whole need is four fields out
/// of one known response. It is a real parser rather than a search for quoted
/// keys: a release description containing a brace or a quote would defeat the
/// latter, and that is exactly the kind of bug that only shows up in production.
struct JsonValue;
using JsonObject = std::map<std::string, std::shared_ptr<JsonValue>>;

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Object, Array } kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    JsonObject object;
    std::vector<std::shared_ptr<JsonValue>> array;
};

/// Null when the text is not valid JSON, or is nested past a sane depth.
std::shared_ptr<JsonValue> parseJson(const std::string& source);

const JsonValue* member(const JsonValue* value, const char* key);
std::string stringMember(const JsonValue* value, const char* key);

/// Compares dotted numeric versions a component at a time, so 1.0.10 is newer
/// than 1.0.9 - which string comparison gets backwards.
bool isNewerVersion(const std::string& candidate, const std::string& current);

/// Days between a yyyy-mm-dd calendar date, read as UTC, and now. -1 when the
/// date is unusable or in the future.
int daysSince(const std::string& date);

}  // namespace diskscan
