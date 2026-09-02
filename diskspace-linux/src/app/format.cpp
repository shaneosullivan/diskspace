#include "format.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

/// The length of the well-formed UTF-8 sequence starting at `text`, or zero.
///
/// Strict, because the point of asking is to decide whether a byte can be passed
/// through untouched: overlong encodings, surrogates and anything above U+10FFFF
/// are rejected, since a renderer handed one of those is entitled to do anything
/// at all with it.
size_t sequenceLength(const char* text, size_t available) {
    const auto byte = [&](size_t i) { return (unsigned char)text[i]; };
    const unsigned char first = byte(0);
    if (first < 0x80) return 1;

    size_t length = 0;
    uint32_t rune = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        rune = first & 0x1Fu;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        rune = first & 0x0Fu;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        rune = first & 0x07u;
    } else {
        return 0;
    }
    if (available < length) return 0;
    for (size_t i = 1; i < length; ++i) {
        if ((byte(i) & 0xC0) != 0x80) return 0;
        rune = (rune << 6) | (byte(i) & 0x3Fu);
    }
    if (length == 2 && rune < 0x80) return 0;
    if (length == 3 && rune < 0x800) return 0;
    if (length == 4 && rune < 0x10000) return 0;
    if (rune > 0x10FFFF) return 0;
    if (rune >= 0xD800 && rune <= 0xDFFF) return 0;
    return length;
}

}  // namespace

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

    // The common case is a name that is already valid, and it should cost one
    // pass and no allocation beyond the copy.
    size_t index = 0;
    while (index < length) {
        const size_t step = sequenceLength(bytes + index, length - index);
        if (step == 0) break;
        index += step;
    }
    if (index == length) return std::string(bytes, length);

    // Something is not valid UTF-8. Everything good is kept and each bad byte is
    // escaped the way ls does it, so the name stays recognisable and the row
    // still appears - which is the whole point, since the alternative is a file
    // that quietly does not show up in a tool for finding files.
    std::string out(bytes, index);
    out.reserve(length + 8);
    while (index < length) {
        const size_t step = sequenceLength(bytes + index, length - index);
        if (step > 0) {
            out.append(bytes + index, step);
            index += step;
            continue;
        }
        char escape[8];
        std::snprintf(escape, sizeof(escape), "\\%03o", (unsigned char)bytes[index]);
        out += escape;
        ++index;
    }
    return out;
}

}  // namespace app
