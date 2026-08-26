#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace diskscan {

std::string toUtf8(const std::wstring& s);
std::string toUtf8(const wchar_t* s, size_t len);
std::wstring fromUtf8(const std::string& s);

/// Prefixes a path with \\?\ so it escapes the 260-character limit.
///
/// The prefix turns off all path parsing, so the path must already be fully
/// qualified with no forward slashes and no . or .. components — which is why
/// this normalises rather than just concatenating. UNC paths take the \\?\UNC\
/// form instead.
std::wstring extendedPath(const std::wstring& path);

/// Fully qualifies and tidies a user-supplied path, without the \\?\ prefix.
/// Returns an empty string when the path cannot be resolved.
std::wstring canonicalPath(const std::wstring& path);

/// True when the current process can read raw volume data, which is what the MFT
/// backend needs.
bool isElevated();

/// Cluster size of the volume holding `path`, or 0 if it cannot be determined.
uint32_t clusterSize(const std::wstring& path);

struct VolumeInfo {
    std::wstring mountPoint;   // e.g. "C:\"
    std::wstring fileSystem;   // e.g. "NTFS"
    int64_t totalBytes = 0;
    int64_t freeBytes = 0;
    uint32_t clusterBytes = 0;
    bool isNtfs = false;
};

/// Volume the path lives on. `ok` is false when it could not be read.
bool volumeInfoForPath(const std::wstring& path, VolumeInfo* out);

/// Every mounted, browsable volume, root volume first and then by capacity.
std::vector<VolumeInfo> mountedVolumes();

/// Whether `path` is a whole volume rather than a folder on one.
///
/// One implementation, because getting it subtly wrong is quiet and expensive: a
/// mount point arrives as "C:\\" and a scan root as either "C:\\" or "C:", and
/// comparing those at face value answers "no" for every drive on the machine.
bool isVolumeRoot(const std::wstring& path);

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
