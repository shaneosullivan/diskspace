#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "name_string.h"

namespace diskscan {

/// The surface every platform has to provide for the scanner and the command
/// line tool to be written once.
///
/// Deliberately small. Anything that only one platform has - the \?\ prefix, an
/// NTFS cluster round-up, /proc parsing - stays in that platform's own header;
/// what is here is what both genuinely need.

/// Fully qualifies and tidies a user-supplied path. Empty when it cannot be
/// resolved.
NameString canonicalPath(const NameString& path);

/// Allocation unit of the filesystem holding `path`, or 0 if unknown.
uint32_t clusterSize(const NameString& path);

/// Whether anything exists at `path` that a scan could be rooted at.
bool pathExists(const NameString& path);

/// The root in the exact form the scan workers prepend to every path they build,
/// so building one is a memcpy rather than a rewrite. Windows applies the prefix
/// that escapes its path length limit here, once, instead of per directory;
/// elsewhere this is the path itself.
NameString workerRootFor(const NameString& canonical);

/// True when the process has the rights its platform's fastest backend needs.
/// Administrator on Windows, root on Linux - where it buys reach into
/// unreadable directories rather than a faster backend, there being none.
bool isElevated();

struct VolumeInfo {
    NameString mountPoint;  // "C:\" or "/"
    NameString fileSystem;  // "NTFS", "ext4"
    int64_t totalBytes = 0;
    /// Free space this account can actually use.
    int64_t freeBytes = 0;
    /// Space in use, which is not `totalBytes - freeBytes`.
    ///
    /// ext4 keeps a reserve only root may write into - five per cent by default,
    /// which is over fifty gigabytes on a terabyte volume. It is neither free to
    /// this account nor occupied by anybody's files, so subtracting free from
    /// total counts it as used and reports a nearly empty disk as full. Asked for
    /// separately, and answered by each platform with whatever it actually means
    /// there.
    int64_t usedBytes = 0;
    uint32_t clusterBytes = 0;
    /// Whether the volume supports the platform's fast whole-volume backend.
    /// Always false on Linux, which has none - see docs/linux-plan.md.
    bool supportsFastScan = false;
};

/// Volume the path lives on. False when it could not be read.
bool volumeInfoForPath(const NameString& path, VolumeInfo* out);

/// Every mounted, browsable volume, root volume first and then by capacity.
///
/// "Browsable" is doing real work on Linux, where /proc/mounts lists dozens of
/// pseudo-filesystems that are not storage and would be nonsense in a disk usage
/// chooser.
std::vector<VolumeInfo> mountedVolumes();

/// The path that scans the whole of `volume`, keeping any trailing separator.
///
/// Exists so there is one answer rather than two. "C:" and "C:\" are not the
/// same thing to any Windows path API: the first means "the current directory on
/// drive C:", so a chooser that trimmed the separator for display and reused the
/// result as a path scanned wherever the process happened to be. It reported a
/// real total for a real folder, which is the worst way for it to be wrong.
NameString volumeScanPath(const VolumeInfo& volume);

/// Whether `path` is a whole volume rather than a folder on one.
bool isVolumeRoot(const NameString& path);

}  // namespace diskscan
