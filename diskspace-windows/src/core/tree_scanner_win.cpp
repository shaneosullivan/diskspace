// The Win32 half of the tree scanner: the clock, the directory open and the
// bulk directory read. Everything else about a scan is in core/tree_scanner.cpp
// and is shared with the Linux build.

#include "tree_scanner.h"

#include <windows.h>

#include "file_kind.h"
#include "win_util.h"

namespace diskscan {

int64_t nowTicks() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double ticksToSeconds(int64_t ticks) {
    static const double scale = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    return double(ticks) * scale;
}

void TreeScanner::workerLoop(DirQueue* dirs) {
    const size_t bufSize = options_.bulkBufferSize;
    void* buffer = _aligned_malloc(bufSize, 16);
    // Reused for the lifetime of the worker: walking a tree allocates nothing.
    std::vector<wchar_t> pathBuffer;
    pathBuffer.reserve(1024);

    int64_t tBuild = 0, tOpen = 0, tRead = 0, tPush = 0;
    int64_t workerStart = nowTicks();

    DirWork node;
    while (dirs->take(&node)) {
        ScanBatch* batch = pool_.obtain();
        batch->reset(node);

        int64_t t = nowTicks();
        buildPath(node, &pathBuffer);
        int64_t t1 = nowTicks();
        tBuild += t1 - t;
        t = t1;

        // FILE_FLAG_BACKUP_SEMANTICS is what makes CreateFileW open a directory
        // at all. Sharing every mode matters on a live system: without
        // FILE_SHARE_DELETE a file being replaced underneath the scan would fail
        // the open and silently lose a whole subtree.
        HANDLE handle = CreateFileW(
            pathBuffer.data(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        int64_t t2 = nowTicks();
        tOpen += t2 - t;
        t = t2;

        if (handle == INVALID_HANDLE_VALUE) {
            batch->errors += 1;
            batch->unreadable = true;
            // Still enqueued, so the directory is accounted for and the walk can
            // terminate.
            batches_->push(batch);
            continue;
        }

        pathBuffer.pop_back();  // drop the terminator; readDirectory appends names
        readDirectory(handle, batch, buffer, bufSize, &pathBuffer);
        CloseHandle(handle);
        int64_t t3 = nowTicks();
        tRead += t3 - t;

        batches_->push(batch);
        tPush += nowTicks() - t3;
    }

    int64_t total = nowTicks() - workerStart;
    _aligned_free(buffer);
    {
        std::lock_guard<std::mutex> lock(stageMutex_);
        tBuildPath_ += tBuild;
        tOpen_ += tOpen;
        tRead_ += tRead;
        tPush_ += tPush;
        tWorkerTotal_ += total;
    }
}

static inline bool isDotOrDotDot(const wchar_t* p, size_t len) {
    if (len == 1) return p[0] == L'.';
    if (len == 2) return p[0] == L'.' && p[1] == L'.';
    return false;
}

void TreeScanner::readDirectory(void* handle, ScanBatch* batch, void* buffer, size_t bufferSize,
                                std::vector<wchar_t>* dirPath) {
    HANDLE h = HANDLE(handle);
    const size_t dirPathLength = dirPath->size();
    const bool resolveCompressed = options_.resolveCompressedSizes;
    const bool dedupe = options_.countHardlinksOnce;

    // FileIdBothDirectoryInfo is the closest thing Windows has to a bulk stat: one
    // call fills the buffer with as many entries as fit, each carrying its name,
    // both sizes, its timestamps, its attributes and its file id. Nothing here
    // needs a per-entry syscall.
    BOOL ok = GetFileInformationByHandleEx(h, FileIdBothDirectoryRestartInfo, buffer,
                                           DWORD(bufferSize));
    while (ok) {
        batch->syscalls += 1;
        auto* info = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buffer);
        for (;;) {
            size_t nameLen = size_t(info->FileNameLength) / sizeof(wchar_t);
            const wchar_t* name = info->FileName;
            const DWORD attributes = info->FileAttributes;

            if (nameLen == 0 || isDotOrDotDot(name, nameLen)) goto nextEntry;

            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                // A directory reparse point is a junction, a symbolic link or a
                // mounted volume. Recorded as an entry, never opened: following
                // one double-counts what it points at and can cycle.
                if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    batch->append(name, nameLen, FileKind::Directory,
                                  NodeFlags::IsDir | NodeFlags::IsMountPoint, 0, 0, false, 0,
                                  epochSecondsFromFileTime(info->CreationTime.QuadPart));
                    goto nextEntry;
                }
                batch->subdirIndices.push_back(int32_t(batch->nodes.size()));
                // NTFS keeps a directory's index in its own MFT record and its
                // allocation is not reported here, so a folder contributes only
                // what is inside it.
                batch->append(name, nameLen, FileKind::Directory, NodeFlags::IsDir, 0, 0, false,
                              0, epochSecondsFromFileTime(info->CreationTime.QuadPart));
                goto nextEntry;
            }

            {
                int64_t logical = info->EndOfFile.QuadPart;
                int64_t allocated = info->AllocationSize.QuadPart;

                // A compressed or sparse file's listed allocation is its size
                // laid out flat, not the clusters it actually holds. Those are
                // the two cases where the number is wrong, and both are rare
                // enough that one extra call apiece costs nothing measurable.
                if (resolveCompressed &&
                    (attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE))) {
                    dirPath->push_back(L'\\');
                    dirPath->insert(dirPath->end(), name, name + nameLen);
                    dirPath->push_back(L'\0');
                    DWORD high = 0;
                    DWORD low = GetCompressedFileSizeW(dirPath->data(), &high);
                    batch->syscalls += 1;
                    if (low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) {
                        allocated = int64_t((uint64_t(high) << 32) | uint64_t(low));
                    }
                    dirPath->resize(dirPathLength);
                }

                if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    // The link itself, not its target: the target is either
                    // outside the tree or already counted where it really lives.
                    batch->append(name, nameLen, FileKind::Symlink, NodeFlags::IsSymlink, 0, 0,
                                  true, 0, epochSecondsFromFileTime(info->CreationTime.QuadPart));
                    goto nextEntry;
                }

                batch->append(name, nameLen, FileKind::classify(name, nameLen), 0, logical,
                              allocated, true,
                              dedupe ? uint64_t(info->FileId.QuadPart) : 0,
                              epochSecondsFromFileTime(info->CreationTime.QuadPart));
            }

        nextEntry:
            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<uint8_t*>(info) +
                                                            info->NextEntryOffset);
        }
        ok = GetFileInformationByHandleEx(h, FileIdBothDirectoryInfo, buffer, DWORD(bufferSize));
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) batch->errors += 1;
}

}  // namespace diskscan
