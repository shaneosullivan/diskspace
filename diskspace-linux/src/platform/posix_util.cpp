#include "posix_util.h"

#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace diskscan {

// A name is already a byte string here, and so is UTF-8 as far as anything in
// this process is concerned. Both conversions are the identity: bytes that are
// not valid UTF-8 pass through unchanged rather than being replaced, so a name
// that came off the disk can always be handed back to open().
std::string toUtf8(const NameString& s) { return s; }
NameString fromUtf8(const std::string& s) { return s; }

NameString canonicalPath(const NameString& path) {
    if (path.empty()) return NameString();
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) {
        // realpath fails when the path does not exist. A trailing separator is
        // still worth removing so "/home/" and "/home" are one answer, and "/"
        // stays "/" rather than becoming empty.
        NameString out = path;
        while (out.size() > 1 && out.back() == kPathSeparator) out.pop_back();
        return out;
    }
    NameString out(resolved);
    ::free(resolved);
    return out;
}

bool pathExists(const NameString& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

NameString workerRootFor(const NameString& canonical) {
    // No prefix to apply: Linux has no path length limit to escape, and the
    // canonical form is already what open() wants.
    return canonical;
}

uint32_t clusterSize(const NameString& path) {
    struct statvfs vfs;
    if (::statvfs(path.c_str(), &vfs) != 0) return 0;
    // f_frsize is the fragment size, which is the unit f_blocks counts in and
    // the one an allocation is actually rounded to. f_bsize is a preferred I/O
    // size and is frequently larger.
    return uint32_t(vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize);
}

bool isElevated() { return ::geteuid() == 0; }

bool isPseudoFileSystem(const std::string& type) {
    static const std::set<std::string> kPseudo = {
        "autofs",     "bpf",         "binfmt_misc", "cgroup",   "cgroup2",
        "configfs",   "debugfs",     "devpts",      "devtmpfs", "efivarfs",
        "fuse.gvfsd-fuse", "fusectl", "hugetlbfs",  "mqueue",   "nsfs",
        "proc",       "pstore",      "ramfs",       "rpc_pipefs", "securityfs",
        "selinuxfs",  "sysfs",       "tracefs",
    };
    return kPseudo.count(type) != 0;
}

namespace {

/// One line of /proc/mounts, which is space separated with octal escapes in the
/// mount point for the characters that would otherwise end a field.
struct MountEntry {
    std::string device;
    std::string mountPoint;
    std::string type;
    std::string options;
};

/// A mount nothing can be freed on, because it is a view of storage counted
/// somewhere else rather than storage of its own.
///
/// overlay is a stack of other filesystems; squashfs is a read-only image, which
/// is how snaps arrive; rootfs is the kernel's initial mount, still present
/// underneath everything. Offering any of them would invite scanning the same
/// bytes twice under two names.
bool isDerivedFileSystem(const std::string& type) {
    return type == "overlay" || type == "rootfs" || type == "squashfs";
}

/// Whether the mount was made read-only, from the option list.
///
/// Nothing can be deleted on one, which is the question a disk usage tool is
/// being asked. It also removes a whole class of system plumbing without naming
/// any of it: read-only bind mounts of drivers, of documentation, of images.
bool isReadOnly(const std::string& options) {
    return options == "ro" || options.rfind("ro,", 0) == 0;
}

std::string unescapeMount(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
            int value = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
            out.push_back(char(value));
            i += 3;
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

std::vector<MountEntry> readMounts() {
    std::vector<MountEntry> out;
    FILE* file = ::fopen("/proc/self/mounts", "re");
    if (!file) return out;
    char line[4096];
    while (::fgets(line, sizeof(line), file)) {
        char device[1024], point[2048], type[256], options[2048];
        if (::sscanf(line, "%1023s %2047s %255s %2047s", device, point, type, options) != 4) {
            continue;
        }
        out.push_back(MountEntry{device, unescapeMount(point), type, options});
    }
    ::fclose(file);
    return out;
}

/// Whether a mount is one a person would recognise as a place their files live.
///
/// Bind mounts and the many mounts a container runtime makes of the same device
/// are filtered by the caller, which keeps one entry per device.
bool isInterestingMount(const MountEntry& entry) {
    if (isPseudoFileSystem(entry.type)) return false;
    if (isDerivedFileSystem(entry.type)) return false;
    // Nothing can be freed on a read-only mount, and this is what removes the
    // system's own plumbing without having to name any of it.
    if (isReadOnly(entry.options)) return false;
    // A tmpfs is memory. /tmp is the one a person puts files in and the one
    // worth offering; /run, /dev/shm and the rest hold runtime state that is
    // gone at the next boot anyway.
    if (entry.type == "tmpfs" && entry.mountPoint != "/tmp") return false;
    // Snaps are read-only images loop-mounted from a file that is already
    // counted where it really lives.
    if (entry.device.rfind("/dev/loop", 0) == 0) return false;
    if (entry.mountPoint.rfind("/snap", 0) == 0) return false;
    return true;
}

bool fillVolume(const MountEntry& entry, VolumeInfo* out) {
    struct statvfs vfs;
    if (::statvfs(entry.mountPoint.c_str(), &vfs) != 0) return false;
    if (vfs.f_blocks == 0) return false;  // nothing to report on
    const uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    out->mountPoint = entry.mountPoint;
    out->fileSystem = entry.type;
    out->totalBytes = int64_t(uint64_t(vfs.f_blocks) * unit);
    // f_bavail rather than f_bfree: the difference is the reserve only root can
    // use, which is not free space to anyone asking this question.
    out->freeBytes = int64_t(uint64_t(vfs.f_bavail) * unit);
    // From f_bfree, not from total minus available: the gap between f_bfree and
    // f_bavail is the root reserve, which nothing has written to.
    out->usedBytes = int64_t(uint64_t(vfs.f_blocks - vfs.f_bfree) * unit);
    out->clusterBytes = uint32_t(unit);
    // No Linux filesystem has a counterpart to reading the MFT directly.
    out->supportsFastScan = false;
    return true;
}

}  // namespace

bool volumeInfoForPath(const NameString& path, VolumeInfo* out) {
    NameString target = canonicalPath(path);
    if (target.empty()) target = path;

    // The mount whose point is the longest prefix of the path is the one the
    // path is actually on - /home before / when both match.
    const std::vector<MountEntry> mounts = readMounts();
    const MountEntry* best = nullptr;
    size_t bestLength = 0;
    for (const MountEntry& entry : mounts) {
        if (isPseudoFileSystem(entry.type)) continue;
        const std::string& point = entry.mountPoint;
        if (target.compare(0, point.size(), point) != 0) continue;
        // A prefix has to end at a component boundary: /home must not match
        // /homework.
        if (point.size() > 1 && target.size() > point.size() &&
            target[point.size()] != kPathSeparator) {
            continue;
        }
        if (point.size() >= bestLength) {
            bestLength = point.size();
            best = &entry;
        }
    }
    if (!best) return false;
    return fillVolume(*best, out);
}

/// Whether this process is inside a Flatpak sandbox.
static bool inSandbox() {
    static const bool inside = ::access("/.flatpak-info", F_OK) == 0;
    return inside;
}

/// What can be offered when sandboxed.
///
/// Inside a Flatpak, /proc/mounts describes the sandbox and not the machine: the
/// runtime is bind-mounted over /usr, /etc and a dozen other places, and the
/// entry that looks most like a root volume is whichever piece of runtime
/// plumbing happens to sort first. Left to itself, the chooser offered
/// /var/cache as though it were the disk, and scanning "/" would have measured
/// the GNOME runtime and called it your files - a plausible-looking number that
/// is entirely wrong, which is the worst kind.
///
/// What genuinely is the host's inside the sandbox is whatever
/// --filesystem=host binds at its real path: the home directory, and anything
/// under /mnt, /media or /run/media. statvfs on those reports the real
/// filesystem, so the figures are right; the list is simply shorter than it
/// would be outside.
static std::vector<VolumeInfo> sandboxVolumes() {
    std::vector<VolumeInfo> out;
    std::set<std::string> seen;

    std::vector<std::string> candidates;
    if (const char* home = ::getenv("HOME"); home && *home) candidates.push_back(home);
    for (const char* parent : {"/mnt", "/media", "/run/media"}) {
        DIR* dir = ::opendir(parent);
        if (!dir) continue;
        while (const struct dirent* entry = ::readdir(dir)) {
            if (entry->d_name[0] == '.') continue;
            candidates.push_back(std::string(parent) + "/" + entry->d_name);
        }
        ::closedir(dir);
    }

    for (const std::string& path : candidates) {
        struct stat st;
        if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        VolumeInfo info;
        // Through the ordinary lookup, which finds the filesystem type from the
        // longest matching mount - the bind of /home does carry its real type.
        if (!volumeInfoForPath(path, &info)) continue;
        if (info.totalBytes <= 0) continue;

        // The same exclusions the unsandboxed list applies. They are worth
        // repeating rather than skipping: /mnt holds WSL's own tmpfs mounts on
        // this machine, and a removable-media directory elsewhere will hold
        // whatever else the system put there.
        if (isPseudoFileSystem(info.fileSystem) || isDerivedFileSystem(info.fileSystem)) {
            continue;
        }
        if (info.fileSystem == "tmpfs" && path != "/tmp") continue;

        // One entry per filesystem, keyed by what it actually is rather than by
        // where it is bound.
        char key[64];
        std::snprintf(key, sizeof(key), "%llu:%lld", (unsigned long long)st.st_dev,
                      (long long)info.totalBytes);
        if (!seen.insert(key).second) continue;

        // The path offered is the one that exists inside the sandbox, not the
        // mount point the lookup matched: scanning /home would reach every
        // account, and only this one is readable.
        info.mountPoint = path;
        out.push_back(info);
    }

    std::sort(out.begin(), out.end(), [](const VolumeInfo& a, const VolumeInfo& b) {
        return a.totalBytes > b.totalBytes;
    });
    return out;
}

std::vector<VolumeInfo> mountedVolumes() {
    if (inSandbox()) return sandboxVolumes();

    std::vector<VolumeInfo> out;
    std::set<std::string> seenDevices;
    for (const MountEntry& entry : readMounts()) {
        if (!isInterestingMount(entry)) continue;
        // One entry per backing device. A bind mount and its origin are the same
        // bytes, and listing both would invite scanning the same disk twice.
        if (entry.device.rfind("/dev/", 0) == 0 && !seenDevices.insert(entry.device).second) {
            continue;
        }
        VolumeInfo info;
        if (fillVolume(entry, &info)) out.push_back(info);
    }
    // Root first, then by capacity, matching what the Windows chooser offers.
    std::sort(out.begin(), out.end(), [](const VolumeInfo& a, const VolumeInfo& b) {
        const bool aRoot = a.mountPoint == "/";
        const bool bRoot = b.mountPoint == "/";
        if (aRoot != bRoot) return aRoot;
        return a.totalBytes > b.totalBytes;
    });
    return out;
}

NameString volumeScanPath(const VolumeInfo& volume) { return volume.mountPoint; }

bool isVolumeRoot(const NameString& path) {
    NameString target = canonicalPath(path);
    if (target.empty()) target = path;
    while (target.size() > 1 && target.back() == kPathSeparator) target.pop_back();
    for (const VolumeInfo& volume : mountedVolumes()) {
        NameString point = volume.mountPoint;
        while (point.size() > 1 && point.back() == kPathSeparator) point.pop_back();
        if (point == target) return true;
    }
    return false;
}

int suggestedScanWorkers(const NameString& root) {
    (void)root;
    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) hardware = 4;

    // Well past the core count, deliberately.
    //
    // This work is not CPU-bound and the measurements say so plainly: at eight
    // workers the pool spends 2.00 of its 2.10 seconds inside read, and the
    // applier is busy for 0.01 of the 0.26 seconds the scan takes. Workers are
    // parked waiting on metadata almost the whole time, so the useful number is
    // however many requests the storage will answer at once, not how many cores
    // there are to run them on.
    //
    // Measured cold on ext4, caches dropped before every run, against the eight
    // the shared default would otherwise choose:
    //
    //     workers   /usr     /
    //     8         1.00x    1.00x
    //     16        1.55x    1.65x
    //     32        2.15x    2.19x
    //     48        2.04x    2.38x
    //
    // Thirty-two is where /usr stops improving and / is still climbing slowly.
    // Taking it as the ceiling gives up a little on the largest trees and avoids
    // spending threads for nothing on the common case.
    //
    // There is deliberately no special case for a spinning disk. The obvious one
    // - back off when /sys says the device is rotational - was written, measured
    // and removed, because the signal cannot be trusted: WSL2 reports every one
    // of its virtual disks as rotational, including the NVMe-backed one this was
    // measured on, and so do plenty of virtio and cloud block devices. Acting on
    // it meant four workers and half the speed on hardware that is not a disk at
    // all. Guessing wrong in that direction is the expensive one, the queue depth
    // is what an elevator wants in order to sort seeks anyway, and anybody who
    // does know their storage can say --workers.
    //
    // Not measured on bare metal: this was a VHDX under WSL2, which is a real
    // read path but a virtualised block device. The shape is clear enough to act
    // on - a factor of two is not a measurement artefact - and the figure is
    // worth re-checking on a physical disk before it is quoted anywhere.
    return int(std::min(32u, std::max(8u, hardware)));
}

}  // namespace diskscan
