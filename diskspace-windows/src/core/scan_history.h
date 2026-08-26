#pragma once

#include <cstdint>
#include <string>

namespace diskscan {

/// Remembers when each root was last scanned, so "added since you last looked"
/// has something to compare against.
///
/// Stored per root path under %LOCALAPPDATA%\DiskSpace. The first scan of a root
/// has no baseline, and that bucket is reported as unavailable rather than as
/// zero — zero would read as "nothing new", which is a different claim.
namespace ScanHistory {

/// Epoch seconds when `root` was last scanned, or 0 if this is the first time.
uint32_t lastScan(const std::wstring& root);
void record(const std::wstring& root, uint32_t whenEpochSeconds = 0);
void forget(const std::wstring& root);

/// %LOCALAPPDATA%\DiskSpace, created if needed. Empty on failure.
std::wstring supportDirectory();

}  // namespace ScanHistory
}  // namespace diskscan
