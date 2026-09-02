#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "name_string.h"

#include "batch.h"
#include "id_map.h"
#include "node_arena.h"
#include "queues.h"

namespace diskscan {

enum class ScanBackend {
    /// Pick the fastest backend the machine and the target allow.
    Auto,
    /// Batched directory enumeration: one `GetFileInformationByHandleEx` call
    /// returns many entries with their sizes and timestamps already filled in.
    /// Works everywhere, needs no privileges.
    Enum,
    /// Reads the NTFS master file table directly. Roughly an order of magnitude
    /// faster on a whole volume, but needs administrator rights and NTFS.
    ///
    /// Windows only. Linux has no counterpart: ext4, btrfs and XFS share no
    /// on-disk layout, so matching this would mean three hand-written filesystem
    /// parsers rather than one. Asking for it there falls back to Enum.
    Mft,
};

const NameChar* scanBackendName(ScanBackend b);

struct ScanProgress {
    int64_t files = 0;
    int64_t directories = 0;
    int64_t nodes = 0;
    int64_t allocatedBytes = 0;
};

struct ScanOptions {
    ScanBackend backend = ScanBackend::Auto;
    /// Directory enumeration is dominated by metadata reads, and past a certain
    /// point concurrent ones interfere at the device level rather than in our
    /// locks. The default is measured by `diskscan bench`; see the README.
    int workers = 0;  // 0 means "choose from the machine"
    /// Reparse points — junctions, symbolic links and mounted volumes — are never
    /// descended into. Following one both double-counts whatever it points at and
    /// can send the walk round a cycle, so they are recorded as entries and left
    /// closed. There is deliberately no option to change that.
    bool countHardlinksOnce = true;
    /// Ask the file system for the true on-disk size of compressed and sparse
    /// files, which the directory listing reports uncompressed. One extra call
    /// per affected file, and nothing at all for ordinary ones.
    bool resolveCompressedSizes = true;
    /// Cutoffs in epoch seconds for the three recency buckets. `hasSinceLastScan`
    /// is false the first time a root is scanned, and that bucket then stays zero
    /// and is reported as unavailable rather than as "nothing new".
    uint32_t cutoffLastMonth = 0;
    uint32_t cutoffLastSixMonths = 0;
    bool hasSinceLastScan = false;
    uint32_t cutoffSinceLastScan = 0;
    /// Bytes handed to each directory-enumeration call.
    size_t bulkBufferSize = 256 * 1024;
    /// Called on a background thread ~10x/second with the running totals.
    std::function<void(const ScanProgress&)> progress;

    ScanOptions();
    /// Fills in the "since last scan" cutoff from recorded history for this root.
    void applyScanHistory(const NameString& root);
    /// Resolves Auto and a zero worker count against this machine and target.
    /// Workers to run, honouring an explicit setting and otherwise asking the
    /// platform what this storage wants. The root is optional only so that a
    /// caller with nothing to scan yet can still report a plausible figure.
    int resolvedWorkers(const NameString& root = NameString()) const;
};

struct ScanResult {
    int64_t files = 0;
    int64_t directories = 0;
    int64_t errors = 0;
    int64_t hardlinkDuplicates = 0;
    int64_t hardlinkDuplicateBytes = 0;
    double elapsed = 0;
    int64_t syscalls = 0;
    /// True when the walk was stopped part way. Everything counted is still
    /// accurate for what was reached; it is simply not the whole tree.
    bool stopped = false;
    /// Zero when this root has never been scanned before, so the UI can say
    /// "no baseline yet" instead of showing a misleading zero.
    uint32_t sinceLastScanBaseline = 0;
    /// How many entries reported a usable creation time, for confidence in the
    /// recency figures.
    int64_t entriesWithTimestamp = 0;
    /// Deepest the applier queue ever got, and how often a worker had to wait on
    /// it. Both should stay near zero; if they don't, the applier is the limit.
    size_t queueMaxDepth = 0;
    size_t queueBlockedPushes = 0;
    /// Stage breakdown, summed across workers (seconds).
    double secBuildPath = 0;
    double secOpen = 0;
    double secRead = 0;
    double secPush = 0;
    double secWorkerTotal = 0;
    /// Applier: time inside apply(), and total lifetime including waiting.
    double secApplier = 0;
    double secApplierTotal = 0;
    /// Which backend actually ran, and whether the faster one was available.
    ScanBackend backend = ScanBackend::Enum;
    /// True when the platform's fast backend was asked for but unavailable.
    bool fastScanUnavailable = false;
    NameString fastScanUnavailableReason;
};

/// Walks a directory tree with several I/O workers feeding one applier thread.
///
/// Workers only read the file system and fill thread-local batches. A single
/// applier owns every mutation of the arena, which means readers (the UI) need no
/// lock at all: they read `arena.count()` and everything below it is immutable in
/// structure and stable in address.
class TreeScanner {
public:
    explicit TreeScanner(const ScanOptions& options = ScanOptions());
    ~TreeScanner();

    NodeArena& arena() { return arena_; }
    const NodeArena& arena() const { return arena_; }

    /// Entries still in the tree: everything the scan found, less whatever has
    /// since been removed.
    ///
    /// Not the same as `arena().count()`, which only ever grows - a removed node
    /// keeps its slot so the removal can be undone. Anything showing the user how
    /// many files and folders there are wants this one; subtracting the rolled-up
    /// file count from `count()` makes the folder count rise when a file is
    /// deleted, which is how this came to exist.
    int64_t liveNodeCount() const { return int64_t(arena_.count()) - removedNodes_; }
    const ScanOptions& options() const { return options_; }

    ScanResult scan(const NameString& root);
    /// Scans one directory again, in place. Everything previously recorded under
    /// `node` is discarded first, so a folder whose contents have been deleted
    /// drops to zero rather than keeping figures nothing on disk supports.
    ScanResult rescan(int32_t node);
    /// What `remove` took away, and everything needed to put it back.
    ///
    /// Removal orphans a subtree rather than freeing it: the nodes stay in the
    /// arena and their parent links stay correct, so all that is lost is the two
    /// fields naming the children and the arithmetic applied to the ancestors.
    /// Keep one of these and the removal can be undone exactly.
    struct Removal {
        int32_t node = -1;
        int32_t childStart = 0;
        int32_t childCount = 0;
        /// The node's own figures, as they were before it was taken out.
        ///
        /// Carried here rather than read back from the arena, because by the time
        /// anything wants to restore them the arena holds zero: removal is what
        /// put it there. Reading instead of remembering was the first way this
        /// was written, and it restored nothing at all.
        int64_t logical = 0;
        int64_t allocated = 0;
        int64_t addedLastMonth = 0;
        int64_t addedLastSixMonths = 0;
        int64_t addedSinceLastScan = 0;
        int32_t fileCount = 0;
        /// Entries the removal took out of the tree, this one included. Kept so
        /// the count of what is left does not have to be recomputed by walking
        /// the whole arena every time the status line is redrawn.
        int64_t nodeCount = 0;
        int16_t kind = 0;
        bool isDirectory = false;
        bool valid = false;
    };

    /// Takes an entry out of every figure entirely, for something that no longer
    /// exists. The node stays in the tree, at zero.
    ///
    /// The returned token restores it. Callers with nothing to undo can ignore
    /// the return value, which is what deleting outright looks like.
    Removal remove(int32_t node);

    /// Puts back what `remove` took. Every figure returns to what it was.
    ///
    /// One thing does not come back: a claim in the hard-link map. Removal
    /// releases the file ids inside the subtree so that rescanning the same
    /// files counts them again rather than treating them as duplicates of
    /// entries that are gone. Restoring does not re-claim them, so a file with
    /// several links that is removed, restored, and then reached again by a
    /// rescan can be counted twice. Nothing on the undo paths this exists for
    /// rescans in between, and re-claiming would need the map to remember what
    /// it had released, which is a cost on every scan for a case that does not
    /// arise.
    void restore(const Removal& token);

    /// Whether `node` lies under `ancestor`, or is it.
    ///
    /// Public because a caller holding several removals needs to know that none
    /// of them contains another: removing a folder already takes everything
    /// inside it out of the figures, so removing one of those as well would
    /// subtract the same bytes twice.
    bool isDescendant(int32_t node, int32_t ancestor) const;

    /// Abandons a scan in progress. Everything applied so far stays valid and
    /// readable, and the workers stop as soon as they finish the directory in
    /// hand.
    void stop();

    struct KindTotal {
        int16_t kind;
        int64_t bytes;
        int64_t count;
    };
    /// Snapshot of the per-kind totals. Safe to call while scanning: the applier
    /// only ever adjusts these, so a reader may see a value slightly behind but
    /// never a partial one.
    std::vector<KindTotal> kindTotals(SizeMetric metric = SizeMetric::Total,
                                      bool useLogicalSize = false) const;

    /// Root of the current scan, without the \\?\ prefix.
    const NameString& rootPath() const { return rootPath_; }

private:
    void walk(DirWork seed);
    void workerLoop(DirQueue* dirs);
    void applierLoop(DirQueue* dirs);
    void apply(ScanBatch* batch, DirQueue* dirs);
    void readDirectory(void* handle, ScanBatch* batch, void* buffer, size_t bufferSize,
                       std::vector<NameChar>* dirPath);
    void buildPath(int32_t node, std::vector<NameChar>* buffer) const;
    void writeRootNode(const NameString& root);
    void discardSubtree(int32_t node);
    /// The arithmetic of removing or restoring a subtree, in one direction or the
    /// other. Written once so the two can never drift apart, which is the whole
    /// risk in an operation that has to be exactly reversible.
    void adjustSubtree(int32_t node, int sign);
    /// The same for the node's own figures, as opposed to its children's. The
    /// figures are passed in rather than read, so this works in both directions.
    void adjustSelf(const Removal& figures, int sign);
    /// Read a node's own figures into a token, ready to be applied either way.
    Removal captureSelf(int32_t node) const;

    ScanResult makeResult(int64_t startTicks) const;
#ifdef _WIN32
    ScanResult runMft(const NameString& root, int64_t startTicks, bool* handled);
#endif

    ScanOptions options_;
    /// How many entries `remove` has taken out and `restore` has not put back.
    int64_t removedNodes_ = 0;

    /// Entries in the subtree rooted at `node`, itself included, skipping
    /// anything already removed - its slots are counted against `removedNodes_`
    /// already, and counting them twice would leave the total short after both
    /// are restored.
    int64_t countSubtree(int32_t node) const;

    NodeArena arena_;
    NameString rootPath_;
    /// The root in the exact form the workers prepend to every path they build,
    /// so that build is a memcpy rather than a string rewrite. On Windows that
    /// means the \\?\ prefix is already applied.
    NameString workerRoot_;
    uint32_t clusterBytes_ = 0;

    BatchQueue* batches_ = nullptr;
    BatchPool pool_;

    // Applier-owned state — no synchronisation needed, single writer.
    IdMap seenIds_;
    int64_t fileTotal_ = 0;
    int64_t directoryTotal_ = 1;  // the root
    int64_t errorTotal_ = 0;
    int64_t syscallTotal_ = 0;
    int64_t hardlinkDupes_ = 0;
    int64_t hardlinkDupBytes_ = 0;
    int64_t timestampedTotal_ = 0;
    /// Per-kind totals maintained as entries are applied, so the file-kind column
    /// redraws in O(kinds) instead of sweeping every node.
    std::vector<int64_t> kindBytes_, kindLogical_, kindMonth_, kindSix_, kindScan_;
    std::vector<int64_t> kindCount_;

    mutable std::mutex stopMutex_;
    bool stopRequested_ = false;
    DirQueue* liveDirs_ = nullptr;
    std::atomic<bool> stopProgress_{false};

    // Where the time actually goes, in ticks.
    std::mutex stageMutex_;
    int64_t tBuildPath_ = 0, tOpen_ = 0, tRead_ = 0, tPush_ = 0, tWorkerTotal_ = 0;
    int64_t tApplier_ = 0, tApplierTotal_ = 0;
    size_t lastQueueMaxDepth_ = 0, lastQueueBlockedPushes_ = 0;
    ScanBackend backendUsed_ = ScanBackend::Enum;
    bool fastScanUnavailable_ = false;
    NameString fastScanUnavailableReason_;
};

/// Ticks and their frequency, for the stage timings.
int64_t nowTicks();
double ticksToSeconds(int64_t ticks);

}  // namespace diskscan
