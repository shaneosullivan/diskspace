#include "tree_scanner.h"


#include <chrono>
#include <thread>
#include <vector>

#include "file_kind.h"
#include "platform.h"
#include "scan_history.h"

namespace diskscan {

const NameChar* scanBackendName(ScanBackend b) {
    switch (b) {
        case ScanBackend::Mft: return DS_T("mft");
        case ScanBackend::Enum: return DS_T("enum");
        default: return DS_T("auto");
    }
}

ScanOptions::ScanOptions() {
    uint32_t now = uint32_t(::time(nullptr));
    cutoffLastMonth = now > 30u * 86400u ? now - 30u * 86400u : 0;
    cutoffLastSixMonths = now > 182u * 86400u ? now - 182u * 86400u : 0;
}

void ScanOptions::applyScanHistory(const NameString& root) {
    uint32_t previous = ScanHistory::lastScan(root);
    hasSinceLastScan = previous != 0;
    cutoffSinceLastScan = previous;
}

int ScanOptions::resolvedWorkers(const NameString& root) const {
    if (workers > 0) return workers;

    // The platform first, where it has an opinion worth having. What the right
    // number is depends on how the storage behaves under concurrent metadata
    // reads, and that is not a question this file can answer for both of them.
    if (!root.empty()) {
        if (const int suggested = suggestedScanWorkers(root); suggested > 0) return suggested;
    }

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

ScanResult TreeScanner::scan(const NameString& root) {
    int64_t started = nowTicks();

    NameString canonical = canonicalPath(root);
    if (canonical.empty()) canonical = root;

    if (!pathExists(canonical)) {
        ScanResult r;
        r.errors = 1;
        r.elapsed = ticksToSeconds(nowTicks() - started);
        return r;
    }

    rootPath_ = canonical;
    workerRoot_ = workerRootFor(canonical);
    clusterBytes_ = clusterSize(canonical);

    writeRootNode(canonical);
    // A fresh tree has had nothing taken out of it, even if this scanner is being
    // reused for a second scan.
    removedNodes_ = 0;

#ifdef _WIN32
    // Linux has no counterpart to the MFT, so there is nothing to try first.
    bool handled = false;
    ScanResult mftResult = runMft(canonical, started, &handled);
    if (handled) {
        if (!mftResult.stopped) ScanHistory::record(canonical);
        return mftResult;
    }
#endif

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
    // The old children are about to be orphaned and re-walked into fresh slots.
    // The abandoned ones keep their place in the arena but are no longer part of
    // the tree, so they count as gone; the folder itself stays and is re-walked.
    removedNodes_ += countSubtree(node) - 1;
    discardSubtree(node);
    walk(node);
    // A refreshed folder is not a scan of the whole root, so it must not move the
    // since-last-scan baseline.
    return makeResult(started);
}

int64_t TreeScanner::countSubtree(int32_t node) const {
    const size_t n = size_t(uint32_t(node));
    if (n >= arena_.count()) return 0;
    if ((arena_.flags[n] & NodeFlags::IsRemoved) != 0) return 0;

    int64_t total = 1;
    // An explicit stack rather than recursion: this runs over whatever the user
    // selected, and a deep tree should not be able to overflow the real one.
    std::vector<int32_t> stack{node};
    while (!stack.empty()) {
        const size_t current = size_t(uint32_t(stack.back()));
        stack.pop_back();
        const int32_t start = arena_.childStart[current];
        const int32_t count = arena_.childCount[current];
        for (int32_t i = 0; i < count; ++i) {
            const int32_t child = start + i;
            // A child already removed took its own descendants with it, and its
            // slots are in removedNodes_ already.
            if ((arena_.flags[size_t(uint32_t(child))] & NodeFlags::IsRemoved) != 0) continue;
            ++total;
            stack.push_back(child);
        }
    }
    return total;
}

TreeScanner::Removal TreeScanner::remove(int32_t node) {
    Removal token;
    token.node = node;
    token.childStart = arena_.childStart[size_t(uint32_t(node))];
    token.childCount = arena_.childCount[size_t(uint32_t(node))];
    // Counted before anything is taken apart: discardSubtree orphans the children
    // and there would be nothing left to walk.
    token.nodeCount = countSubtree(node);

    // The children first, and everything they contributed. That leaves the entry
    // holding only its own figures, which is the moment to read them.
    discardSubtree(node);

    const Removal figures = captureSelf(node);
    token.logical = figures.logical;
    token.allocated = figures.allocated;
    token.addedLastMonth = figures.addedLastMonth;
    token.addedLastSixMonths = figures.addedLastSixMonths;
    token.addedSinceLastScan = figures.addedSinceLastScan;
    token.fileCount = figures.fileCount;
    token.kind = figures.kind;
    token.isDirectory = figures.isDirectory;

    adjustSelf(token, -1);
    // Out of the figures, and out of the list. Without this the entry would sit
    // there at zero bytes, which reads as an empty folder rather than a gone one.
    arena_.flags[size_t(uint32_t(node))] |= NodeFlags::IsRemoved;
    removedNodes_ += token.nodeCount;
    arena_.publish();

    token.valid = true;
    return token;
}

void TreeScanner::restore(const Removal& token) {
    if (!token.valid || token.node < 0) return;
    const size_t n = size_t(uint32_t(token.node));
    if (n >= arena_.count()) return;

    // The node's own figures first, while it still holds nothing, and then the
    // children - the reverse of the order removal took them in.
    adjustSelf(token, +1);
    arena_.flags[n] &= uint8_t(~NodeFlags::IsRemoved);
    removedNodes_ -= token.nodeCount;
    arena_.childStart[n] = token.childStart;
    arena_.childCount[n] = token.childCount;
    adjustSubtree(token.node, +1);
    arena_.publish();
}

void TreeScanner::adjustSubtree(int32_t node, int sign) {
    const size_t n = size_t(node);

    // Sum what the children contributed, rather than taking the directory's own
    // total: that total also includes the directory entry itself, which is not
    // part of the subtree and must survive.
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
        arena_.logical[a] += sign * logical;
        arena_.allocated[a] += sign * allocated;
        arena_.fileCount[a] += sign * files;
        arena_.addedLastMonth[a] += sign * month;
        arena_.addedLastSixMonths[a] += sign * six;
        arena_.addedSinceLastScan[a] += sign * sinceScan;
        if (a == 0) break;
        a = size_t(uint32_t(arena_.parent[a]));
    }
    fileTotal_ += sign * files;

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
            kindBytes_[k] += sign * arena_.allocated[child];
            kindLogical_[k] += sign * arena_.logical[child];
            kindCount_[k] += sign;
            kindMonth_[k] += sign * arena_.addedLastMonth[child];
            kindSix_[k] += sign * arena_.addedLastSixMonths[child];
            kindScan_[k] += sign * arena_.addedSinceLastScan[child];
        }
    }
    directoryTotal_ += sign * directories;
}

TreeScanner::Removal TreeScanner::captureSelf(int32_t node) const {
    const size_t n = size_t(uint32_t(node));
    Removal out;
    out.node = node;
    out.logical = arena_.logical[n];
    out.allocated = arena_.allocated[n];
    out.fileCount = arena_.fileCount[n];
    out.addedLastMonth = arena_.addedLastMonth[n];
    out.addedLastSixMonths = arena_.addedLastSixMonths[n];
    out.addedSinceLastScan = arena_.addedSinceLastScan[n];
    out.kind = arena_.kind[n];
    out.isDirectory = (arena_.flags[n] & NodeFlags::IsDir) != 0;
    return out;
}

void TreeScanner::adjustSelf(const Removal& figures, int sign) {
    // Its own figures come off every ancestor including itself, which is what
    // leaves the entry in the tree at zero rather than merely emptied.
    size_t a = size_t(uint32_t(figures.node));
    for (;;) {
        arena_.logical[a] += sign * figures.logical;
        arena_.allocated[a] += sign * figures.allocated;
        arena_.fileCount[a] += sign * figures.fileCount;
        arena_.addedLastMonth[a] += sign * figures.addedLastMonth;
        arena_.addedLastSixMonths[a] += sign * figures.addedLastSixMonths;
        arena_.addedSinceLastScan[a] += sign * figures.addedSinceLastScan;
        if (a == 0) break;
        a = size_t(uint32_t(arena_.parent[a]));
    }

    // adjustSubtree covers the descendants' per-kind figures but not the entry
    // itself, which only counts when it is a file.
    if (!figures.isDirectory) {
        size_t k = size_t(uint32_t(figures.kind));
        kindBytes_[k] += sign * figures.allocated;
        kindLogical_[k] += sign * figures.logical;
        kindCount_[k] += sign;
        kindMonth_[k] += sign * figures.addedLastMonth;
        kindSix_[k] += sign * figures.addedLastSixMonths;
        kindScan_[k] += sign * figures.addedSinceLastScan;
        fileTotal_ += sign;
    } else {
        directoryTotal_ += sign;
    }
}

void TreeScanner::discardSubtree(int32_t node) {
    adjustSubtree(node, -1);

    // Release the file-id claims made inside the subtree, or the files that hold
    // them would be treated as duplicates on the way back in and their bytes
    // would vanish.
    if (options_.countHardlinksOnce && !seenIds_.empty()) {
        seenIds_.retain([&](int32_t owner) { return !isDescendant(owner, node); });
    }

    arena_.childStart[size_t(node)] = 0;
    arena_.childCount[size_t(node)] = 0;
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

void TreeScanner::writeRootNode(const NameString& root) {
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

    int workerCount = options_.resolvedWorkers(rootPath_);
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
    r.fastScanUnavailable = fastScanUnavailable_;
    r.fastScanUnavailableReason = fastScanUnavailableReason_;
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
void TreeScanner::buildPath(int32_t node, std::vector<NameChar>* buffer) const {
    buffer->clear();
    buffer->insert(buffer->end(), workerRoot_.begin(), workerRoot_.end());
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
            if (buffer->back() != kPathSeparator) buffer->push_back(kPathSeparator);
            const NameChar* name = arena_.nameData(n);
            buffer->insert(buffer->end(), name, name + arena_.nameSize(n));
        }
    }
    buffer->push_back(NameChar(0));
}


}  // namespace diskscan
