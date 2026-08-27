#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "platform.h"

namespace diskscan {

/// The Linux side of what core/platform.h asks every platform for, plus the few
/// things only this build needs.

/// Rounds a byte count up to a whole number of allocation units.
///
/// Rarely the right question on Linux, where `statx` reports the blocks a file
/// actually holds and that already accounts for tails, sparse holes and
/// filesystem compression. Kept for the one case it is needed: a file whose
/// block count could not be read at all.
inline int64_t roundToCluster(int64_t bytes, uint32_t cluster) {
    if (bytes <= 0 || cluster == 0) return bytes > 0 ? bytes : 0;
    int64_t c = int64_t(cluster);
    return ((bytes + c - 1) / c) * c;
}

/// Filesystems that are not storage, by name, as they appear in /proc/mounts.
///
/// A disk usage tool that offered to scan `proc` or `cgroup2` would be offering
/// nonsense: they report no size, hold no bytes, and walking some of them blocks
/// forever. The list is of what to exclude rather than what to include, because
/// a real filesystem this build has never heard of should still be scannable.
bool isPseudoFileSystem(const std::string& type);

}  // namespace diskscan
