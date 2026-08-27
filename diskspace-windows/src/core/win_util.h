#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "platform.h"

namespace diskscan {

/// The two-argument form, for a name held as a pointer and a length rather than
/// as a string. Shared code never needs it; the Windows scanner does, on a path
/// it has just built into a buffer.
std::string toUtf8(const wchar_t* s, size_t len);

/// Prefixes a path with \?\ so it escapes the 260-character limit.
///
/// The prefix turns off all path parsing, so the path must already be fully
/// qualified with no forward slashes and no . or .. components — which is why
/// this normalises rather than just concatenating. UNC paths take the \?\UNC\
/// form instead.
std::wstring extendedPath(const std::wstring& path);

/// Seconds since the Unix epoch for a FILETIME expressed as a 64-bit value.
/// Returns 0 for a missing or pre-epoch timestamp, which reads as "unknown"
/// everywhere it is used.
inline uint32_t epochSecondsFromFileTime(int64_t fileTime) {
    // FILETIME counts 100ns ticks from 1601-01-01; 11644473600 seconds separate
    // that from the Unix epoch.
    if (fileTime <= 0) return 0;
    int64_t seconds = fileTime / 10000000LL - 11644473600LL;
    if (seconds <= 0) return 0;
    if (seconds > int64_t(UINT32_MAX)) return 0;
    return uint32_t(seconds);
}

/// Rounds a byte count up to a whole number of clusters, which is what the file
/// actually occupies. Zero stays zero: an empty or fully resident file occupies
/// no cluster of its own.
inline int64_t roundToCluster(int64_t bytes, uint32_t cluster) {
    if (bytes <= 0 || cluster == 0) return bytes > 0 ? bytes : 0;
    int64_t c = int64_t(cluster);
    return ((bytes + c - 1) / c) * c;
}

}  // namespace diskscan
