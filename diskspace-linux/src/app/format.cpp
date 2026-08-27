#include "format.h"

#include <glib.h>

#include <cstdio>
#include <cstring>

namespace app {

std::string humanBytes(int64_t bytes) {
    static const char* kUnits[] = {"B", "kB", "MB", "GB", "TB", "PB"};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1000.0 && unit < 5) {
        value /= 1000.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%lld B", (long long)bytes);
    } else if (value < 10.0) {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, kUnits[unit]);
    } else if (value < 100.0) {
        std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, kUnits[unit]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.0f %s", value, kUnits[unit]);
    }
    return buffer;
}

std::string grouped(int64_t value) {
    char digits[32];
    std::snprintf(digits, sizeof(digits), "%lld", (long long)(value < 0 ? -value : value));
    const size_t length = std::strlen(digits);
    std::string out;
    if (value < 0) out.push_back('-');
    for (size_t i = 0; i < length; ++i) {
        if (i > 0 && (length - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

std::string displayName(const char* bytes, size_t length) {
    if (length == 0) return std::string();
    if (g_utf8_validate(bytes, gssize(length), nullptr)) return std::string(bytes, length);

    // Not valid UTF-8. Everything up to the first bad byte is kept, and the rest
    // is escaped the way ls does it, so the name is still recognisable and the
    // row still appears.
    std::string out;
    out.reserve(length + 8);
    for (size_t i = 0; i < length;) {
        const char* start = bytes + i;
        const char* end = nullptr;
        if (g_utf8_validate(start, gssize(length - i), &end) || end > start) {
            const size_t good = size_t(end - start);
            out.append(start, good);
            i += good;
            if (i >= length) break;
        }
        char escape[8];
        std::snprintf(escape, sizeof(escape), "\\%03o", (unsigned char)bytes[i]);
        out += escape;
        ++i;
    }
    return out;
}

}  // namespace app
