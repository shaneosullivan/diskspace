#include "tree_scanner.h"

#include <windows.h>

#include <chrono>
#include <thread>

#include "file_kind.h"
#include "scan_history.h"
#include "win_util.h"

namespace diskscan {

const wchar_t* scanBackendName(ScanBackend b) {
    switch (b) {
        case ScanBackend::Mft: return L"mft";
        case ScanBackend::Enum: return L"enum";
        default: return L"auto";
    }
}

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

ScanOptions::ScanOptions() {
    uint32_t now = uint32_t(::time(nullptr));
    cutoffLastMonth = now > 30u * 86400u ? now - 30u * 86400u : 0;
    cutoffLastSixMonths = now > 182u * 86400u ? now - 182u * 86400u : 0;
}

void ScanOptions::applyScanHistory(const std::wstring& root) {
    uint32_t previous = ScanHistory::lastScan(root);
    hasSinceLastScan = previous != 0;
    cutoffSinceLastScan = previous;
}

int ScanOptions::resolvedWorkers() const {
    if (workers > 0) return workers;
    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) hardware = 4;
    // One core is left for the applier and one for the UI. Eight is the measured
    // knee on NVMe: past that, concurrent metadata reads interfere at the device
    // level rather than in our locks.
    int n = int(hardware) - 2;
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    return n;
}

// MARK: - Construction

TreeScanner::TreeScanner(const ScanOptions& options) : options_(options) {
    FileKind::warmUp();
    size_t slots = size_t(FileKind::count());
    kindBytes_.assign(slots, 0);
    kindLogical_.assign(slots, 0);
    kindMonth_.assign(slots, 0);
    kindSix_.assign(slots, 0);
    kindScan_.assign(slots, 0);
    kindCount_.assign(slots, 0);
}

TreeScanner::~TreeScanner() { delete batches_; }

std::vector<TreeScanner::KindTotal> TreeScanner::kindTotals(SizeMetric metric,
                                                            bool useLogicalSize) const {
    const std::vector<int64_t>* source = &kindBytes_;
    switch (metric) {
        case SizeMetric::AddedLastMonth: source = &kindMonth_; break;
        case SizeMetric::AddedLastSixMonths: source = &kindSix_; break;
        case SizeMetric::AddedSinceLastScan: source = &kindScan_; break;
        default: source = useLogicalSize ? &kindLogical_ : &kindBytes_; break;
    }
    std::vector<KindTotal> out;
    out.reserve(kindCount_.size());
    for (size_t k = 0; k < kindCount_.size(); ++k) {
        if (kindCount_[k] > 0) out.push_back({int16_t(k), (*source)[k], kindCount_[k]});
    }
    return out;
}

void TreeScanner::stop() {
    DirQueue* dirs = nullptr;
    {
        std::lock_guard<std::mutex> lock(stopMutex_);
        stopRequested_ = true;
        dirs = liveDirs_;
    }
    if (dirs) dirs->cancel();
}

// MARK: - Entry points

ScanResult TreeScanner::scan(const std::wstring& root) {
    int64_t started = nowTicks();

    std::wstring canonical = canonicalPath(root);
    if (canonical.empty()) canonical = root;

    DWORD attributes = GetFileAttributesW(extendedPath(canonical).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        ScanResult r;
        r.errors = 1;
        r.elapsed = ticksToSeconds(nowTicks() - started);
        return r;
    }

    rootPath_ = canonical;
    extendedRoot_ = extendedPath(canonical);
    clusterBytes_ = clusterSize(canonical);

    writeRootNode(canonical);

    bool handled = false;
    ScanResult mftResult = runMft(canonical, started, &handled);
    if (handled) {
        if (!mftResult.stopped) ScanHistory::record(canonical);
        return mftResult;
    }

    backendUsed_ = ScanBackend::Enum;
    walk(0);

    ScanResult result = makeResult(started);
    // Recorded only on a completed scan, so an aborted run cannot move the
    // baseline forward and hide changes from the next comparison.
    if (!result.stopped) ScanHistory::record(canonical);
    return result;
}

ScanResult TreeScanner::rescan(int32_t node) {
    int64_t started = nowTicks();
    discardSubtree(node);
    walk(node);
    // A refreshed folder is not a scan of the whole root, so it must not move the
    // since-last-scan baseline.
    return makeResult(started);
}

void TreeScanner::remove(int32_t node) {
    // Clears the children and everything they contributed, leaving the entry
    // holding only its own.
    discardSubtree(node);

    size_t n = size_t(node);
    int64_t logical = arena_.logical[n];
    int64_t allocated = arena_.allocated[n];
    int32_t files = arena_.fileCount[n];
    int64_t month = arena_.addedLastMonth[n];
    int64_t six = arena_.addedLastSixMonths[n];
    int64_t sinceScan = arena_.addedSinceLastScan[n];

    size_t a = n;
    for (;;) {
        arena_.logical[a] -= logical;
        arena_.allocated[a] -= allocated;
        arena_.fileCount[a] -= files;
        arena_.addedLastMonth[a] -= month;
        arena_.addedLastSixMonths[a] -= six;
        arena_.addedSinceLastScan[a] -= sinceScan;
        if (a == 0) break;
        a = size_t(uint32_t(arena_.parent[a]));
    }

    // discardSubtree covers the descendants' per-kind figures but not the entry
    // itself, which only counts when it is a file.
    if ((arena_.flags[n] & NodeFlags::IsDir) == 0) {
        size_t k = size_t(arena_.kind[n]);
        kindBytes_[k] -= allocated;
        kindLogical_[k] -= logical;
        kindCount_[k] -= 1;
        kindMonth_[k] -= month;
        kindSix_[k] -= six;
        kindScan_[k] -= sinceScan;
        fileTotal_ -= 1;
    } else {
        directoryTotal_ -= 1;
    }
    arena_.publish();
}

// MARK: - Undoing work already applied

void TreeScanner::discardSubtree(int32_t node) {
    size_t n = size_t(node);

    // Sum what the children contributed, rather than taking the directory's own
    // total: that total also includes the directory entry itself, which is not
    // being rescanned and must survive.
    int64_t logical = 0, allocated = 0, month = 0, six = 0, sinceScan = 0;
    int32_t files = 0;
    size_t firstChild = size_t(uint32_t(arena_.childStart[n]));
    size_t childCount = size_t(uint32_t(arena_.childCount[n]));
    for (size_t child = firstChild; child < firstChild + childCount; ++child) {
        logical += arena_.logical[child];
        allocated += arena_.allocated[child];
        files += arena_.fileCount[child];
        month += arena_.addedLastMonth[child];
        six += arena_.addedLastSixMonths[child];
        sinceScan += arena_.addedSinceLastScan[child];
    }

    size_t a = n;
    for (;;) {
        arena_.logical[a] -= logical;
        arena_.allocated[a] -= allocated;
        arena_.fileCount[a] -= files;
        arena_.addedLastMonth[a] -= month;
        arena_.addedLastSixMonths[a] -= six;
        arena_.addedSinceLastScan[a] -= sinceScan;
        if (a == 0) break;
        a = size_t(uint32_t(arena_.parent[a]));
    }
    fileTotal_ -= files;

    // The per-kind figures are sums of individual files, so those have to be
    // visited one by one.
    std::vector<int32_t> stack{node};
    int64_t directories = 0;
    while (!stack.empty()) {
        size_t current = size_t(uint32_t(stack.back()));
        stack.pop_back();
        size_t start = size_t(uint32_t(arena_.childStart[current]));
        size_t count = size_t(uint32_t(arena_.childCount[current]));
        for (size_t child = start; child < start + count; ++child) {
            if (arena_.flags[child] & NodeFlags::IsDir) {
                ++directories;
                stack.push_back(int32_t(child));
                continue;
            }
            size_t k = size_t(arena_.kind[child]);
            kindBytes_[k] -= arena_.allocated[child];
            kindLogical_[k] -= arena_.logical[child];
            kindCount_[k] -= 1;
            kindMonth_[k] -= arena_.addedLastMonth[child];
            kindSix_[k] -= arena_.addedLastSixMonths[child];
            kindScan_[k] -= arena_.addedSinceLastScan[child];
        }
    }
    directoryTotal_ -= directories;

    // Release the file-id claims made inside the subtree, or the files that hold
    // them would be treated as duplicates on the way back in and their bytes
    // would vanish.
    if (options_.countHardlinksOnce && !seenIds_.empty()) {
        seenIds_.retain([&](int32_t owner) { return !isDescendant(owner, node); });
    }

    arena_.childStart[n] = 0;
    arena_.childCount[n] = 0;
    arena_.publish();
}

/// Walks the parent chain, which is why orphaning a subtree is safe: parents stay
/// correct for nodes nothing points at any more.
bool TreeScanner::isDescendant(int32_t node, int32_t ancestor) const {
    if (node == ancestor) return true;
    int32_t current = node;
    while (current != 0) {
        current = arena_.parent[size_t(uint32_t(current))];
        if (current == ancestor) return true;
    }
    return false;
}

// MARK: - Driving the walk

void TreeScanner::writeRootNode(const std::wstring& root) {
    arena_.reserveNodes(1);
    size_t off = arena_.allocateName(root.size());
    arena_.writeName(root.data(), root.size(), off);
    arena_.parent[0] = 0;
    arena_.nameOffset[0] = int32_t(off);
    arena_.nameLength[0] = uint16_t(root.size() > 0xFFFF ? 0xFFFF : root.size());
    arena_.kind[0] = FileKind::Directory;
    arena_.flags[0] = NodeFlags::IsDir;
    arena_.publish();
}

void TreeScanner::walk(DirWork seed) {
    {
        std::lock_guard<std::mutex> lock(stopMutex_);
        stopRequested_ = false;
    }
    delete batches_;
    batches_ = new BatchQueue();
    stopProgress_.store(false);

    DirQueue dirs(seed);
    bool alreadyStopped = false;
    {
        std::lock_guard<std::mutex> lock(stopMutex_);
        alreadyStopped = stopRequested_;
        liveDirs_ = &dirs;
    }
    // stop() may have been called before this thread got that far.
    if (alreadyStopped) dirs.cancel();

    std::thread progressThread;
    if (options_.progress) {
        progressThread = std::thread([this] {
            while (!stopProgress_.load(std::memory_order_relaxed)) {
                // Lock-free: readers only need the published count and the arena.
                size_t nodes = arena_.count();
                ScanProgress p;
                p.files = arena_.fileCount[0];
                p.directories = int64_t(nodes) - p.files;
                p.nodes = int64_t(nodes);
                p.allocatedBytes = arena_.allocated[0];
                options_.progress(p);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    int workerCount = options_.resolvedWorkers();
    std::vector<std::thread> workers;
    workers.reserve(size_t(workerCount));
    for (int i = 0; i < workerCount; ++i) {
        workers.emplace_back([this, &dirs] { workerLoop(&dirs); });
    }
    std::thread applier([this, &dirs] { applierLoop(&dirs); });

    for (auto& t : workers) t.join();
    batches_->close();
    applier.join();

    stopProgress_.store(true);
    if (progressThread.joinable()) progressThread.join();
    arena_.publish();

    lastQueueMaxDepth_ = batches_->maxDepth();
    lastQueueBlockedPushes_ = batches_->blockedPushes();
    {
        std::lock_guard<std::mutex> lock(stopMutex_);
        liveDirs_ = nullptr;
    }
}

ScanResult TreeScanner::makeResult(int64_t startTicks) const {
    bool wasStopped;
    {
        std::lock_guard<std::mutex> lock(stopMutex_);
        wasStopped = stopRequested_;
    }
    ScanResult r;
    r.files = fileTotal_;
    r.directories = directoryTotal_;
    r.errors = errorTotal_;
    r.syscalls = syscallTotal_;
    r.entriesWithTimestamp = timestampedTotal_;
    r.sinceLastScanBaseline = options_.hasSinceLastScan ? options_.cutoffSinceLastScan : 0;
    r.hardlinkDuplicates = hardlinkDupes_;
    r.hardlinkDuplicateBytes = hardlinkDupBytes_;
    r.secBuildPath = ticksToSeconds(tBuildPath_);
    r.secOpen = ticksToSeconds(tOpen_);
    r.secRead = ticksToSeconds(tRead_);
    r.secPush = ticksToSeconds(tPush_);
    r.secWorkerTotal = ticksToSeconds(tWorkerTotal_);
    r.secApplier = ticksToSeconds(tApplier_);
    r.secApplierTotal = ticksToSeconds(tApplierTotal_);
    r.queueMaxDepth = lastQueueMaxDepth_;
    r.queueBlockedPushes = lastQueueBlockedPushes_;
    r.elapsed = ticksToSeconds(nowTicks() - startTicks);
    r.stopped = wasStopped;
    r.backend = backendUsed_;
    r.mftUnavailable = mftUnavailable_;
    r.mftUnavailableReason = mftUnavailableReason_;
    return r;
}

// MARK: - Applier (single writer)

void TreeScanner::applierLoop(DirQueue* dirs) {
    int64_t start = nowTicks();
    int64_t busy = 0;
    for (;;) {
        ScanBatch* batch = batches_->take();
        if (!batch) break;
        int64_t t0 = nowTicks();
        apply(batch, dirs);
        busy += nowTicks() - t0;
        pool_.recycle(batch);
    }
    tApplier_ = busy;
    tApplierTotal_ = nowTicks() - start;
}

void TreeScanner::apply(ScanBatch* batch, DirQueue* dirs) {
    errorTotal_ += batch->errors;
    syscallTotal_ += batch->syscalls;

    size_t n = batch->nodes.size();
    size_t parentNode = size_t(uint32_t(batch->parentNode));
    if (batch->unreadable) arena_.flags[parentNode] |= NodeFlags::IsUnreadable;

    if (n > 0) {
        size_t base = arena_.reserveNodes(n);

        int64_t files = 0, bytes = 0, logicalBytes = 0;
        int64_t bytesMonth = 0, bytesSixMonths = 0, bytesSinceScan = 0;
        const uint32_t cutoffMonth = options_.cutoffLastMonth;
        const uint32_t cutoffSix = options_.cutoffLastSixMonths;
        const bool hasScanCutoff = options_.hasSinceLastScan;
        const uint32_t cutoffScan = options_.cutoffSinceLastScan;
        const bool dedupe = options_.countHardlinksOnce;

        for (size_t i = 0; i < n; ++i) {
            const PendingNode& pn = batch->nodes[i];
            size_t idx = base + i;
            int64_t lg = pn.logical;
            int64_t al = pn.allocated;
            int32_t fc = pn.fileCount;
            uint8_t fl = pn.flags;

            // Hard-link suppression happens before anything is rolled up, so a
            // file reachable by several paths contributes its bytes once and
            // running totals are never transiently inflated.
            if (dedupe && pn.fileId != 0 && !seenIds_.claim(pn.fileId, int32_t(idx))) {
                ++hardlinkDupes_;
                hardlinkDupBytes_ += al;
                lg = 0;
                al = 0;
                fc = 0;
                fl |= NodeFlags::IsHardlinkDup;
            }

            size_t len = pn.nameLength;
            size_t off = arena_.allocateName(len);
            arena_.writeName(batch->names.data() + size_t(uint32_t(pn.nameOffset)), len, off);

            arena_.parent[idx] = batch->parentNode;
            arena_.nameOffset[idx] = int32_t(off);
            arena_.nameLength[idx] = pn.nameLength;
            arena_.kind[idx] = pn.kind;
            arena_.flags[idx] = fl;
            arena_.logical[idx] = lg;
            arena_.allocated[idx] = al;
            arena_.fileCount[idx] = fc;
            arena_.childStart[idx] = 0;
            arena_.childCount[idx] = 0;
            arena_.addedTime[idx] = pn.addedTime;

            // Bucket this entry's own bytes, then roll the batch totals up.
            // Suppressed hard links already have al == 0, so they contribute
            // nothing here either.
            int64_t inMonth = 0, inSix = 0, inScan = 0;
            if (pn.addedTime > 0) {
                ++timestampedTotal_;
                if (pn.addedTime >= cutoffSix) {
                    inSix = al;
                    if (pn.addedTime >= cutoffMonth) inMonth = al;
                }
                if (hasScanCutoff && pn.addedTime >= cutoffScan) inScan = al;
            }
            arena_.addedLastMonth[idx] = inMonth;
            arena_.addedLastSixMonths[idx] = inSix;
            arena_.addedSinceLastScan[idx] = inScan;
            bytesMonth += inMonth;
            bytesSixMonths += inSix;
            bytesSinceScan += inScan;

            if ((fl & NodeFlags::IsDir) == 0) {
                size_t k = size_t(pn.kind);
                kindBytes_[k] += al;
                kindLogical_[k] += lg;
                kindCount_[k] += 1;
                kindMonth_[k] += inMonth;
                kindSix_[k] += inSix;
                kindScan_[k] += inScan;
            }

            files += fc;
            bytes += al;
            logicalBytes += lg;
        }

        fileTotal_ += files;
        directoryTotal_ += int64_t(batch->subdirIndices.size());

        // Children of a directory are one contiguous run, which is what makes the
        // tree view's child lookup O(1) with no auxiliary structure.
        arena_.childStart[parentNode] = int32_t(base);
        arena_.childCount[parentNode] = int32_t(n);

        // Roll this directory's contents into every ancestor. Single writer, so
        // no read-modify-write race is possible.
        size_t a = parentNode;
        for (;;) {
            arena_.logical[a] += logicalBytes;
            arena_.allocated[a] += bytes;
            arena_.fileCount[a] += int32_t(files);
            arena_.addedLastMonth[a] += bytesMonth;
            arena_.addedLastSixMonths[a] += bytesSixMonths;
            arena_.addedSinceLastScan[a] += bytesSinceScan;
            if (a == 0) break;
            a = size_t(uint32_t(arena_.parent[a]));
        }

        arena_.publish();

        std::vector<DirWork> children;
        children.reserve(batch->subdirIndices.size());
        for (int32_t local : batch->subdirIndices) children.push_back(int32_t(base) + local);
        dirs->push(children);
    }

    dirs->completed();
}

// MARK: - Workers (read-only, never touch the arena's structure)

/// Rebuilds an absolute path from the arena into a caller-owned buffer.
///
/// Safe without synchronisation: the applier publishes a node before it queues
/// that node's children, so every name on the ancestor chain is already visible
/// and immutable by the time a worker asks for it.
void TreeScanner::buildPath(int32_t node, std::vector<wchar_t>* buffer) const {
    buffer->clear();
    buffer->insert(buffer->end(), extendedRoot_.begin(), extendedRoot_.end());
    if (node != 0) {
        // Collected first because the chain runs child-to-parent and the path
        // has to be written parent-to-child.
        static thread_local std::vector<int32_t> chain;
        chain.clear();
        size_t cur = size_t(uint32_t(node));
        while (cur > 0) {
            chain.push_back(int32_t(cur));
            cur = size_t(uint32_t(arena_.parent[cur]));
        }
        for (size_t i = chain.size(); i-- > 0;) {
            size_t n = size_t(uint32_t(chain[i]));
            if (buffer->back() != L'\\') buffer->push_back(L'\\');
            const wchar_t* name = arena_.nameData(n);
            buffer->insert(buffer->end(), name, name + arena_.nameSize(n));
        }
    }
    buffer->push_back(L'\0');
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
