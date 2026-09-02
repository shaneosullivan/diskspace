// The Linux half of the tree scanner: the clock, the directory open and the
// directory read. Everything else about a scan is in core/tree_scanner.cpp and
// is shared with the Windows build.
//
// The shape differs from Win32 in one way that matters. There,
// GetFileInformationByHandleEx fills a buffer with entries that already carry
// their sizes, timestamps, attributes and file id - a bulk stat, one call for
// many files. Linux has no such call: getdents64 returns names and little else,
// so each entry needs its own statx. That is the cost of the platform and there
// is no way around it, but two things keep it as cheap as it can be:
//
//   * statx is issued against the open directory's descriptor with a relative
//     name, so the kernel walks no path at all - no lookup of each ancestor per
//     file, which is what a plain stat() of a full path would pay.
//   * AT_STATX_DONT_SYNC says the cached answer is good enough. On a network
//     filesystem the alternative is a round trip per file.

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "file_kind.h"
#include "posix_util.h"
#include "tree_scanner.h"

namespace diskscan {

int64_t nowTicks() {
    struct timespec ts;
    // MONOTONIC rather than REALTIME: a scan's elapsed time should not change
    // because ntp stepped the clock in the middle of it.
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + int64_t(ts.tv_nsec);
}

double ticksToSeconds(int64_t ticks) { return double(ticks) * 1e-9; }

namespace {

inline bool isDotOrDotDot(const char* p) {
    if (p[0] != '.') return false;
    if (p[1] == '\0') return true;
    return p[1] == '.' && p[2] == '\0';
}

/// One key per file for the hard-link map, from the pair that actually
/// identifies a file on Linux.
///
/// An inode number alone is not unique across a system - two filesystems
/// each have an inode 2 - so the device has to be mixed in or files on one
/// would suppress unrelated files on another. Zero is reserved by IdMap to mean
/// "empty", so it is nudged aside.
inline uint64_t fileKey(uint64_t device, uint64_t inode) {
    uint64_t x = inode * 0x9E3779B97F4A7C15ULL ^ (device + 0x165667B19E3779F9ULL);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x ? x : 1;
}

inline uint64_t deviceOf(const struct statx& s) {
    return (uint64_t(s.stx_dev_major) << 32) | uint64_t(s.stx_dev_minor);
}

/// When the entry came into being, in epoch seconds.
///
/// Birth time is the honest answer and the one the Windows build uses, but no
/// filesystem is obliged to record it and several do not. Change time is the
/// fallback: it moves when a file is written or its metadata changes, so for a
/// file that arrived and was left alone - which is exactly the case the recency
/// buckets are looking for - it is the same answer.
inline uint32_t addedTimeOf(const struct statx& s) {
    const int64_t seconds =
        (s.stx_mask & STATX_BTIME) ? int64_t(s.stx_btime.tv_sec) : int64_t(s.stx_ctime.tv_sec);
    if (seconds <= 0 || seconds > int64_t(UINT32_MAX)) return 0;
    return uint32_t(seconds);
}

}  // namespace

void TreeScanner::workerLoop(DirQueue* dirs) {
    const size_t bufSize = options_.bulkBufferSize;
    void* buffer = ::aligned_alloc(16, (bufSize + 15) & ~size_t(15));
    // Reused for the lifetime of the worker: walking a tree allocates nothing.
    std::vector<NameChar> pathBuffer;
    pathBuffer.reserve(1024);

    int64_t tBuild = 0, tOpen = 0, tRead = 0, tPush = 0;
    const int64_t workerStart = nowTicks();

    DirWork node;
    while (dirs->take(&node)) {
        ScanBatch* batch = pool_.obtain();
        batch->reset(node);

        int64_t t = nowTicks();
        buildPath(node, &pathBuffer);
        const int64_t t1 = nowTicks();
        tBuild += t1 - t;
        t = t1;

        // O_NOFOLLOW guards against the one race that would otherwise let the
        // walk escape: a directory swapped for a symlink between being listed
        // and being opened. Nothing reached here is a symlink by design - those
        // are recorded and never queued - so it can only fire on that race.
        const int fd = ::open(pathBuffer.data(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        const int64_t t2 = nowTicks();
        tOpen += t2 - t;
        t = t2;

        if (fd < 0) {
            batch->errors += 1;
            batch->unreadable = true;
            // Still enqueued, so the directory is accounted for and the walk can
            // terminate.
            batches_->push(batch);
            continue;
        }

        pathBuffer.pop_back();  // drop the terminator, as the Win32 side does
        readDirectory(reinterpret_cast<void*>(intptr_t(fd)), batch, buffer, bufSize,
                      &pathBuffer);
        ::close(fd);
        const int64_t t3 = nowTicks();
        tRead += t3 - t;

        batches_->push(batch);
        tPush += nowTicks() - t3;
    }

    const int64_t total = nowTicks() - workerStart;
    ::free(buffer);
    {
        std::lock_guard<std::mutex> lock(stageMutex_);
        tBuildPath_ += tBuild;
        tOpen_ += tOpen;
        tRead_ += tRead;
        tPush_ += tPush;
        tWorkerTotal_ += total;
    }
}

void TreeScanner::readDirectory(void* handle, ScanBatch* batch, void* buffer, size_t bufferSize,
                                std::vector<NameChar>* dirPath) {
    (void)dirPath;  // statx goes through the descriptor, so no path is built
    const int fd = int(intptr_t(handle));
    const bool dedupe = options_.countHardlinksOnce;

    // The device this directory is on. An entry reporting a different one is a
    // mount point: recorded, never descended into. The Windows build does the
    // same with a mounted volume, which arrives there as a reparse point, and
    // for the same reason - following it double-counts a disk that is scannable
    // in its own right, and can send the walk round a cycle.
    struct statx dirStat;
    uint64_t dirDevice = 0;
    if (::statx(fd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_TYPE, &dirStat) == 0) {
        dirDevice = deviceOf(dirStat);
        batch->syscalls += 1;
    }

    for (;;) {
        const long got = ::syscall(SYS_getdents64, fd, buffer, bufferSize);
        if (got < 0) {
            batch->errors += 1;
            return;
        }
        if (got == 0) return;
        batch->syscalls += 1;

        for (long offset = 0; offset < got;) {
            auto* entry = reinterpret_cast<struct dirent64*>(static_cast<char*>(buffer) + offset);
            offset += entry->d_reclen;

            const char* name = entry->d_name;
            if (isDotOrDotDot(name)) continue;
            const size_t nameLen = std::strlen(name);
            if (nameLen == 0) continue;

            struct statx st;
            const int rc = ::statx(fd, name,
                                   AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC | AT_NO_AUTOMOUNT,
                                   STATX_TYPE | STATX_SIZE | STATX_BLOCKS | STATX_NLINK |
                                       STATX_INO | STATX_BTIME | STATX_CTIME,
                                   &st);
            batch->syscalls += 1;
            if (rc != 0) {
                // Gone between being listed and being asked about, or not
                // readable. Recorded at zero rather than dropped, so the entry
                // still appears in the tree.
                batch->append(name, nameLen, FileKind::Unknown, NodeFlags::IsUnreadable, 0, 0,
                              true, 0, 0);
                batch->errors += 1;
                continue;
            }

            const uint32_t added = addedTimeOf(st);

            if (S_ISLNK(st.stx_mode)) {
                // The link itself, not its target: the target is either outside
                // the tree or already counted where it really lives.
                //
                // Its own blocks are counted though, which is where this parts
                // company with the Windows build. A reparse point keeps its data
                // inside the MFT record and occupies nothing of its own, so that
                // side reports zero. ext4 stores a target of up to 59 bytes
                // inside the inode the same way, but a longer one gets a data
                // block like any other file - so a tree of long symlinks really
                // does occupy space, and reporting zero would quietly lose it.
                batch->append(name, nameLen, FileKind::Symlink, NodeFlags::IsSymlink,
                              int64_t(st.stx_size), int64_t(st.stx_blocks) * 512, true, 0, added);
                continue;
            }

            if (S_ISDIR(st.stx_mode)) {
                if (dirDevice != 0 && deviceOf(st) != dirDevice) {
                    batch->append(name, nameLen, FileKind::Directory,
                                  NodeFlags::IsDir | NodeFlags::IsMountPoint, 0, 0, false, 0,
                                  added);
                    continue;
                }
                batch->subdirIndices.push_back(int32_t(batch->nodes.size()));
                // A directory's own blocks are its index, not its contents, and
                // counting them would attribute the filesystem's bookkeeping to
                // the user's files. It contributes only what is inside it, which
                // is what the Windows build reports too.
                batch->append(name, nameLen, FileKind::Directory, NodeFlags::IsDir, 0, 0, false,
                              0, added);
                continue;
            }

            if (!S_ISREG(st.stx_mode)) {
                // Sockets, fifos and device nodes occupy no data blocks. They are
                // real entries and are listed, at zero.
                batch->append(name, nameLen, FileKind::Unknown, 0, 0, 0, true, 0, added);
                continue;
            }

            // stx_blocks is always in 512-byte units, whatever the filesystem's
            // own block size, and it is what the file actually holds. Sparse
            // holes are already absent from it and btrfs compression is already
            // accounted for, so none of the fixing-up the NTFS path needs
            // applies here.
            const int64_t logical = int64_t(st.stx_size);
            const int64_t allocated = int64_t(st.stx_blocks) * 512;

            // Only a file with more than one link can be a duplicate, and asking
            // the map about the rest would fill it with an entry per file for
            // nothing. This is what the MFT backend does, and what the macOS
            // build does with st_nlink.
            const uint64_t id =
                (dedupe && st.stx_nlink > 1) ? fileKey(deviceOf(st), st.stx_ino) : 0;

            batch->append(name, nameLen, FileKind::classify(name, nameLen), 0, logical, allocated,
                          true, id, added);
        }
    }
}

}  // namespace diskscan
