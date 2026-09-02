#pragma once

#include <cstdint>
#include <string>

#include "name_string.h"

namespace diskscan {

/// Remembers when each root was last scanned, so "added since you last looked"
/// has something to compare against.
///
/// Stored per root path in the platform's own place for such things:
/// %LOCALAPPDATA%\DiskSpace on Windows, $XDG_DATA_HOME/diskspace on Linux. The
/// first scan of a root has no baseline, and that bucket is reported as
/// unavailable rather than as zero — zero would read as "nothing new", which is
/// a different claim.
namespace ScanHistory {

/// Epoch seconds when `root` was last scanned, or 0 if this is the first time.
uint32_t lastScan(const NameString& root);
void record(const NameString& root, uint32_t whenEpochSeconds = 0);
void forget(const NameString& root);

/// That directory, created if needed. Empty on failure.
NameString supportDirectory();

}  // namespace ScanHistory
}  // namespace diskscan
