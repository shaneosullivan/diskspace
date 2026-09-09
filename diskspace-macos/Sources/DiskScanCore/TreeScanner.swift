import Darwin
import Foundation

public enum ScanBackend: String, CaseIterable {
    /// One `getattrlistbulk` syscall per batch of entries.
    case bulk
    /// Baseline: `readdir` + `fstatat` per entry.
    case stat
}

public struct ScanOptions {
    public var backend: ScanBackend = .bulk
    /// Twice the core count, between 8 and 32.
    ///
    /// Four used to be the default, from measuring a tree that was entirely local
    /// and already cached, where the number makes almost no difference. On a tree
    /// with cloud-backed folders in it — Dropbox, iCloud Drive — it makes all the
    /// difference, because a worker there is parked in the file provider for
    /// whole seconds and four of them park together. Measured on 972k entries
    /// under Dropbox, and on ~/Library for the local case:
    ///
    ///     workers   Dropbox        ~/Library
    ///     4          67,695/s      143,755/s
    ///     8         116,009/s
    ///     16        142,715/s
    ///     20        145,434/s      144,157/s
    ///     32        147,995/s
    ///
    /// So: 2.15x on the tree that was slow, and nothing measurable lost on the
    /// one that was already fast. This is not CPU work — the workers are waiting,
    /// not computing — which is why the useful number is well past the core
    /// count. The Linux build reached the same conclusion from its own
    /// measurements and clamps the same way.
    ///
    /// Not adjusted for spinning disks, which want far fewer. Linux reads that
    /// from sysfs; there is no equally cheap answer here, and every Mac still
    /// shipping has an SSD inside it.
    public var workers: Int = min(
        32, max(8, ProcessInfo.processInfo.activeProcessorCount * 2))
    public var crossMountPoints = false
    public var countHardlinksOnce = true
    /// Cutoffs in epoch seconds for the three recency buckets. `sinceLastScan` is
    /// nil the first time a root is scanned, and that bucket then stays zero and is
    /// reported as unavailable rather than as "nothing new".
    public var cutoffLastMonth: UInt32 = 0
    public var cutoffLastSixMonths: UInt32 = 0
    public var cutoffSinceLastScan: UInt32?
    /// Bytes handed to each `getattrlistbulk` call.
    public var bulkBufferSize = 256 * 1024
    public var attributeProfile: AttributeProfile = .full
    /// Walk a sync provider's tree, or record it and move on.
    ///
    /// Off by default, because a provider's folder is not necessarily empty of
    /// real bytes: files that have been downloaded occupy this disk like any
    /// other, and refusing to look at them would under-report the very thing the
    /// app exists to report. Skipping is for the case where the walk is costing
    /// minutes, which the scan offers to do once it can see that happening.
    public var skipCloudFolders = false
    /// Extra directories to treat as sync-provider roots.
    ///
    /// Empty in the app, where roots are found by asking the filesystem. A test
    /// has no provider to point at and cannot create one, so this is how the
    /// skipping path gets exercised at all — which matters more than it sounds,
    /// since that path decides whether a whole tree is counted or not.
    public var additionalCloudRoots: [String] = []
    /// Called on a background thread ~10x/second with the running totals.
    public var progress: ((ScanProgress) -> Void)?

    public init() {
        let now = Date()
        cutoffLastMonth = UInt32(max(0, now.addingTimeInterval(-30 * 86400).timeIntervalSince1970))
        cutoffLastSixMonths = UInt32(max(0, now.addingTimeInterval(-182 * 86400).timeIntervalSince1970))
    }

    /// Fills in the "since last scan" cutoff from recorded history for this root.
    public mutating func applyScanHistory(for root: String) {
        cutoffSinceLastScan = ScanHistory.lastScan(of: root).map {
            UInt32(max(0, $0.timeIntervalSince1970))
        }
    }
}

public struct ScanProgress {
    public var files: Int
    public var directories: Int
    public var nodes: Int
    public var allocatedBytes: Int64
    /// A directory a worker is inside right now, or nil between batches. One of
    /// several, picked arbitrarily: the point is to show that something is
    /// happening and where, not to account for every worker.
    public var currentPath: String?
    /// True when no entry has been recorded for a while, which on a tree with
    /// cloud-backed folders means a worker is waiting on the file provider
    /// rather than that anything has gone wrong.
    public var stalled: Bool = false
    /// The folder everything is waiting on belongs to a sync provider. Together
    /// with `stalled`, this is the app's cue to offer to leave it alone: it can
    /// then say what is slow and why, rather than only that something is.
    public var stalledInCloudFolder: Bool = false
}

public struct ScanResult {
    public var arena: NodeArena
    public var files = 0
    public var directories = 0
    public var errors = 0
    public var hardlinkDuplicates = 0
    public var hardlinkDuplicateBytes: Int64 = 0
    public var elapsed: Double = 0
    public var syscalls = 0
    /// True when the walk was stopped part way. Everything counted is still
    /// accurate for what was reached; it is simply not the whole tree.
    public var stopped = false
    /// Nil when this root has never been scanned before, so the UI can say
    /// "no baseline yet" instead of showing a misleading zero.
    public var sinceLastScanBaseline: Date?
    /// How many entries reported a usable added/creation time, for confidence in
    /// the recency figures.
    public var entriesWithTimestamp = 0
    /// Workers still inside a provider's syscall when the walk gave up waiting
    /// for them. Non-zero means the tree below those directories is missing.
    public var abandonedWorkers = 0
    /// Sync-provider folders whose contents are not in these totals, counted
    /// per provider rather than per directory inside one. Non-zero means what is
    /// inside them is missing, which for a folder someone syncs is most of it.
    public var cloudFoldersSkipped = 0
    /// Deepest the applier queue ever got, and how often a worker had to wait on
    /// it. Both should stay near zero; if they don't, the applier is the limit.
    public var queueMaxDepth = 0
    public var queueBlockedPushes = 0
    /// Stage breakdown, summed across workers (seconds).
    public var secBuildPath = 0.0
    public var secOpen = 0.0
    public var secRead = 0.0
    public var secPush = 0.0
    public var secWorkerTotal = 0.0
    /// Applier: time inside apply(), and total lifetime including waiting.
    public var secApplier = 0.0
    public var secApplierTotal = 0.0
}

/// Walks a directory tree with several I/O workers feeding one applier thread.
///
/// Workers only read the file system and fill thread-local batches. A single
/// applier owns every mutation of the arena, which means readers (the UI) need no
/// lock at all: they read `arena.count` and everything below it is immutable in
/// structure and stable in address.
public final class TreeScanner {
    private let options: ScanOptions
    public let arena = NodeArena()

    /// Replaced for every walk: a queue is closed when its walk ends, and a
    /// refresh needs one that is open again.
    private var batches = BatchQueue()
    private let pool = BatchPool()

    // Applier-owned state — no synchronisation needed, single writer.
    /// Inode → the node that counted it. A plain set would do for a single pass,
    /// but refreshing one folder has to give back the claims made inside it, and
    /// that needs to know where each claim came from.
    private var seenMultiLinkInodes = [UInt64: Int32]()
    private var fileTotal = 0
    private var directoryTotal = 1  // the root
    private var errorTotal = 0
    private var syscallTotal = 0
    private var hardlinkDupes = 0
    private var hardlinkDupBytes: Int64 = 0
    private var timestampedTotal = 0
    /// Per-kind totals maintained as entries are applied. The file-kind column can
    /// therefore redraw in O(kinds) instead of sweeping every node.
    private let kindBytesStore: UnsafeMutablePointer<Int64>
    private let kindLogicalStore: UnsafeMutablePointer<Int64>
    private let kindMonthStore: UnsafeMutablePointer<Int64>
    private let kindSixStore: UnsafeMutablePointer<Int64>
    private let kindScanStore: UnsafeMutablePointer<Int64>
    private let kindCountStore: UnsafeMutablePointer<Int>
    private let kindSlots: Int

    private var rootDev: dev_t = 0
    /// Which directory each worker is reading, and when it started. Only ever
    /// read by the progress reporter, to say where a scan has got to and to
    /// notice when a worker has been waiting on one directory for a while —
    /// which is what a cloud-backed folder looks like from here.
    private let progressLock = NSLock()
    private var workerNode: [Int32] = []
    private var workerSince: [UInt64] = []
    /// Set from another thread by stop(). Guards the queue reference too, since
    /// it only exists while a scan is running.
    private let stopLock = NSLock()
    private var stopRequested = false
    private var liveDirs: DirQueue?
    /// Provider roots this scan has met, so their descendants are recognised
    /// without asking the filesystem about each one again.
    private let cloudRoots: CloudRootRegistry
    /// Live, because the offer to skip appears mid-scan: the whole point is to
    /// stop a walk that is already underway and going nowhere. Guarded by
    /// `stopLock`, which is the lock the workers already consult.
    private var skipCloudLive = false
    /// Which sync-provider folders came out incomplete, by the node of the
    /// provider's own root rather than of each directory underneath it.
    ///
    /// Counting the directories would be accurate and useless: leaving one
    /// provider alone reported "2054 online folders not counted", which is the
    /// count of everything inside it. One provider is one thing a person can do
    /// something about. Applier-owned, like the other totals.
    private(set) var incompleteCloudRoots = Set<Int32>()
    /// Which walk is current. A worker still inside a syscall when its walk was
    /// abandoned compares this against the epoch it was started with, and knows
    /// to drop what it is holding rather than hand it to whatever is running
    /// now. Without it a stale directory could be grafted into the next scan's
    /// tree — wrong sizes under the wrong parent, and nothing to show why.
    private var walkEpoch = 0
    /// Workers left behind, still inside a syscall that will not return for a
    /// while. Reported so the totals can say they are incomplete.
    private var abandonedWorkers = 0
    /// Directories a worker was still inside when the walk gave up on it.
    private var abandonedNodes: [Int32] = []
    private let abandonedNodesLock = NSLock()
    /// Whether each worker is currently inside a sync provider's folder. Read by
    /// `walk` to decide whether the only thing left is waiting on a provider.
    private var workerInCloud: [Bool] = []
    // Where the time actually goes, in nanoseconds.
    private let stageLock = NSLock()
    private var nsBuildPath: UInt64 = 0
    private var nsOpen: UInt64 = 0
    private var nsRead: UInt64 = 0
    private var nsPush: UInt64 = 0
    private var nsWorkerTotal: UInt64 = 0
    private var nsApplier: UInt64 = 0
    private var nsApplierTotal: UInt64 = 0
    /// Read by the progress thread and written by the scan thread, so it is
    /// behind a lock like everything else those two share.
    private var stopProgress = false
    private let progressStopLock = NSLock()

    private var shouldStopProgress: Bool {
        progressStopLock.lock()
        defer { progressStopLock.unlock() }
        return stopProgress
    }

    private func setStopProgress(_ stop: Bool) {
        progressStopLock.lock()
        stopProgress = stop
        progressStopLock.unlock()
    }

    public init(options: ScanOptions = ScanOptions()) {
        self.options = options
        cloudRoots = CloudRootRegistry(extraRoots: options.additionalCloudRoots)
        FileKind.warmUp()
        kindSlots = FileKind.names.count
        kindBytesStore = .allocate(capacity: kindSlots)
        kindBytesStore.initialize(repeating: 0, count: kindSlots)
        kindLogicalStore = .allocate(capacity: kindSlots)
        kindLogicalStore.initialize(repeating: 0, count: kindSlots)
        kindMonthStore = .allocate(capacity: kindSlots)
        kindMonthStore.initialize(repeating: 0, count: kindSlots)
        kindSixStore = .allocate(capacity: kindSlots)
        kindSixStore.initialize(repeating: 0, count: kindSlots)
        kindScanStore = .allocate(capacity: kindSlots)
        kindScanStore.initialize(repeating: 0, count: kindSlots)
        kindCountStore = .allocate(capacity: kindSlots)
        kindCountStore.initialize(repeating: 0, count: kindSlots)
    }

    deinit {
        kindBytesStore.deallocate()
        kindLogicalStore.deallocate()
        kindMonthStore.deallocate()
        kindSixStore.deallocate()
        kindScanStore.deallocate()
        kindCountStore.deallocate()
    }

    /// Snapshot of the per-kind totals. Safe to call while scanning: the applier
    /// only ever increases these, so a reader may see a value slightly behind but
    /// never a partial one.
    public func kindTotals(metric: SizeMetric = .total, useLogicalSize: Bool = false)
        -> [(kind: Int16, bytes: Int64, count: Int)]
    {
        let source: UnsafeMutablePointer<Int64>
        switch metric {
        case .total: source = useLogicalSize ? kindLogicalStore : kindBytesStore
        case .addedLastMonth: source = kindMonthStore
        case .addedLastSixMonths: source = kindSixStore
        case .addedSinceLastScan: source = kindScanStore
        }
        var out: [(Int16, Int64, Int)] = []
        out.reserveCapacity(kindSlots)
        for k in 0..<kindSlots where kindCountStore[k] > 0 {
            out.append((Int16(k), source[k], kindCountStore[k]))
        }
        return out
    }

    /// Abandons a scan in progress. Everything applied so far stays valid and
    /// readable — the arena is untouched — and the workers stop as soon as they
    /// finish the directory in hand.
    ///
    /// Cancelling the directory queue is all it takes: the workers' only source
    /// of work returns nil, so they wind down without a flag to check on every
    /// iteration.
    public func stop() {
        stopLock.lock()
        stopRequested = true
        let dirs = liveDirs
        stopLock.unlock()
        dirs?.cancel()
    }

    /// Turns skipping on or off while the scan is running.
    ///
    /// Only affects directories not yet read. A provider tree already walked
    /// keeps the figures it contributed, which is right: those bytes were
    /// measured and are real.
    public func setSkipCloudFolders(_ skip: Bool) {
        stopLock.lock()
        skipCloudLive = skip
        stopLock.unlock()
    }

    public var isSkippingCloudFolders: Bool {
        stopLock.lock()
        defer { stopLock.unlock() }
        return skipCloudLive
    }

    public func scan(root: String) -> ScanResult {
        let started = DispatchTime.now()
        setSkipCloudFolders(options.skipCloudFolders)

        var rootBytes = Array(root.utf8)
        while rootBytes.count > 1 && rootBytes.last == 0x2F { rootBytes.removeLast() }
        rootBytes.append(0)

        var st = stat()
        let ok = rootBytes.withUnsafeBufferPointer { buf in
            buf.baseAddress!.withMemoryRebound(to: CChar.self, capacity: buf.count) {
                lstat($0, &st) == 0
            }
        }
        guard ok else {
            var r = ScanResult(arena: arena)
            r.errors = 1
            return r
        }
        rootDev = st.st_dev

        writeRootNode(named: root)
        // A fresh tree has had nothing taken out of it, even if this scanner is
        // being reused for a second scan.
        removedNodes = 0

        walk(seed: 0)

        var result = makeResult(started: started)
        // Recorded only on a completed scan, so an aborted run cannot move the
        // baseline forward and hide changes from the next comparison.
        if !result.stopped { ScanHistory.record(root: root) }
        return result
    }

    /// Scans one directory again, in place.
    ///
    /// Everything previously recorded under `node` is discarded: its totals come
    /// back out of every ancestor, its files come out of the per-kind figures,
    /// and the hard links it claimed are released so the new walk can count them
    /// again. The old nodes themselves stay in the arena — it only ever grows —
    /// but nothing references them once the directory points at its new children.
    /// How many entries `remove` has taken out and `restore` has not put back.
    private var removedNodes: Int64 = 0

    /// Entries still in the tree: everything the scan found, less whatever has
    /// since been removed.
    ///
    /// Not the same as `arena.count`, which only ever grows - a removed node
    /// keeps its slot so the removal can be undone. Anything showing the user how
    /// many files and folders there are wants this one; subtracting the rolled-up
    /// file count from `count` makes the folder count rise when a file is
    /// deleted, which is how this came to exist.
    public var liveNodeCount: Int64 { Int64(arena.count) - removedNodes }

    /// Entries in the subtree rooted at `node`, itself included, skipping
    /// anything already removed - its slots are counted against `removedNodes`
    /// already, and counting them twice would leave the total short after both
    /// are restored.
    private func countSubtree(_ node: Int32) -> Int64 {
        let n = Int(node)
        guard n < arena.count, arena.flags[n] & NodeFlags.isRemoved == 0 else { return 0 }

        var total: Int64 = 1
        // An explicit stack rather than recursion: this runs over whatever the
        // user selected, and a deep tree should not be able to overflow the real
        // one.
        var stack: [Int32] = [node]
        while let current = stack.popLast() {
            let c = Int(current)
            let start = arena.childStart[c]
            let count = arena.childCount[c]
            for i in 0..<count {
                let child = start + i
                // A child already removed took its own descendants with it, and
                // its slots are in removedNodes already.
                if arena.flags[Int(child)] & NodeFlags.isRemoved != 0 { continue }
                total += 1
                stack.append(child)
            }
        }
        return total
    }

    public func rescan(node: Int32) -> ScanResult {
        let started = DispatchTime.now()
        // The old children are about to be orphaned and re-walked into fresh
        // slots. The abandoned ones keep their place in the arena but are no
        // longer part of the tree, so they count as gone; the folder itself stays
        // and is re-walked.
        setSkipCloudFolders(options.skipCloudFolders)
        removedNodes += countSubtree(node) - 1
        discardSubtree(under: node)
        // It is about to be read again, so whatever a previous walk decided not
        // to open no longer applies. Left set, the row would go on saying it was
        // never scanned while showing the bytes this walk just counted.
        arena.flags[Int(node)] &= ~NodeFlags.isCloudUnread
        walk(seed: node)
        // A refreshed folder is not a scan of the whole root, so it must not move
        // the since-last-scan baseline.
        return makeResult(started: started)
    }

    /// Takes an entry out of every figure entirely, for something that no longer
    /// exists — a file or folder just moved to the trash.
    ///
    /// The node stays in the tree, at zero: it is still a row the user is looking
    /// at, and having it drop to nothing is the point.
    /// What `remove` took away, and everything needed to put it back.
    ///
    /// Removal orphans a subtree rather than freeing it: the nodes stay in the
    /// arena and their parent links stay correct, so all that is lost is the two
    /// fields naming the children and the arithmetic applied to the ancestors.
    /// Keep one of these and the removal can be undone exactly.
    public struct Removal {
        var node: Int32 = -1
        var childStart: Int32 = 0
        var childCount: Int32 = 0
        /// The node's own figures, as they were before it was taken out.
        ///
        /// Carried here rather than read back from the arena, because by the time
        /// anything wants to restore them the arena holds zero: removal is what
        /// put it there.
        var logical: Int64 = 0
        var allocated: Int64 = 0
        var addedLastMonth: Int64 = 0
        var addedLastSixMonths: Int64 = 0
        var addedSinceLastScan: Int64 = 0
        var fileCount: Int32 = 0
        /// Entries the removal took out of the tree, this one included. Kept so
        /// the count of what is left does not have to be recomputed by walking
        /// the whole arena every time the status line is redrawn.
        var nodeCount: Int64 = 0
        var kind: Int16 = 0
        var isDirectory = false
        var valid = false

        public init() {}
    }

    /// Takes an entry out of every figure entirely, for something that no longer
    /// exists. The node stays in the tree, at zero.
    ///
    /// The returned token restores it. Callers with nothing to undo can ignore
    /// the return value, which is what deleting outright looks like.
    @discardableResult
    public func remove(node: Int32) -> Removal {
        var token = Removal()
        token.node = node
        token.childStart = arena.childStart[Int(node)]
        token.childCount = arena.childCount[Int(node)]
        // Counted before anything is taken apart: discardSubtree orphans the
        // children and there would be nothing left to walk.
        token.nodeCount = countSubtree(node)

        // The children first, and everything they contributed. That leaves the
        // entry holding only its own figures, which is the moment to read them.
        discardSubtree(under: node)

        let n = Int(node)
        token.logical = arena.logical[n]
        token.allocated = arena.allocated[n]
        token.fileCount = arena.fileCount[n]
        token.addedLastMonth = arena.addedLastMonth[n]
        token.addedLastSixMonths = arena.addedLastSixMonths[n]
        token.addedSinceLastScan = arena.addedSinceLastScan[n]
        token.kind = arena.kind[n]
        token.isDirectory = arena.flags[n] & NodeFlags.isDir != 0

        adjustSelf(token, sign: -1)
        // Out of the figures, and out of the outline. Without this the entry
        // would sit there at zero bytes, which reads as an empty folder rather
        // than a gone one.
        arena.flags[n] |= NodeFlags.isRemoved
        removedNodes += token.nodeCount
        arena.publish()

        token.valid = true
        return token
    }

    /// Puts back what `remove` took. Every figure returns to what it was.
    ///
    /// One thing does not come back: a claim in the hard-link map. Removal
    /// releases the inodes inside the subtree so that rescanning the same files
    /// counts them again rather than treating them as duplicates of entries that
    /// are gone. Nothing on the undo paths this exists for rescans in between.
    public func restore(_ token: Removal) {
        guard token.valid, token.node >= 0, Int(token.node) < arena.count else { return }
        let n = Int(token.node)

        // The node's own figures first, while it still holds nothing, and then
        // the children - the reverse of the order removal took them in.
        adjustSelf(token, sign: 1)
        arena.flags[n] &= ~NodeFlags.isRemoved
        removedNodes -= token.nodeCount
        arena.childStart[n] = token.childStart
        arena.childCount[n] = token.childCount
        adjustSubtree(under: token.node, sign: 1)
        arena.publish()
    }

    /// The arithmetic of removing or restoring a subtree, in one direction or the
    /// other. Written once so the two can never drift apart, which is the whole
    /// risk in an operation that has to be exactly reversible.
    private func adjustSubtree(under node: Int32, sign: Int64) {
        let n = Int(node)

        // Sum what the children contributed, rather than taking the directory's
        // own total: that total also includes the directory entry itself, which
        // is not part of the subtree and must survive.
        var logical: Int64 = 0
        var allocated: Int64 = 0
        var files: Int32 = 0
        var month: Int64 = 0
        var six: Int64 = 0
        var sinceScan: Int64 = 0
        let firstChild = Int(arena.childStart[n])
        let childCount = Int(arena.childCount[n])
        if childCount > 0 {
            for child in firstChild..<(firstChild + childCount) {
                logical += arena.logical[child]
                allocated += arena.allocated[child]
                files += arena.fileCount[child]
                month += arena.addedLastMonth[child]
                six += arena.addedLastSixMonths[child]
                sinceScan += arena.addedSinceLastScan[child]
            }
        }

        var a = n
        while true {
            arena.logical[a] += sign * logical
            arena.allocated[a] += sign * allocated
            arena.fileCount[a] += Int32(sign) * files
            arena.addedLastMonth[a] += sign * month
            arena.addedLastSixMonths[a] += sign * six
            arena.addedSinceLastScan[a] += sign * sinceScan
            if a == 0 { break }
            a = Int(arena.parent[a])
        }
        fileTotal += Int(sign) * Int(files)

        // The per-kind figures are sums of individual files, so those have to be
        // visited one by one.
        var stack = [node]
        var directories = 0
        while let current = stack.popLast() {
            let c = Int(current)
            let start = Int(arena.childStart[c])
            let count = Int(arena.childCount[c])
            guard count > 0 else { continue }
            for child in start..<(start + count) {
                if arena.flags[child] & NodeFlags.isDir != 0 {
                    directories += 1
                    stack.append(Int32(child))
                    continue
                }
                let k = Int(arena.kind[child])
                kindBytesStore[k] += sign * arena.allocated[child]
                kindLogicalStore[k] += sign * arena.logical[child]
                kindCountStore[k] += Int(sign)
                kindMonthStore[k] += sign * arena.addedLastMonth[child]
                kindSixStore[k] += sign * arena.addedLastSixMonths[child]
                kindScanStore[k] += sign * arena.addedSinceLastScan[child]
            }
        }
        directoryTotal += Int(sign) * directories
    }

    /// The same for the node's own figures, as opposed to its children's. The
    /// figures are passed in rather than read, so this works in both directions.
    private func adjustSelf(_ figures: Removal, sign: Int64) {
        // Its own figures come off every ancestor including itself, which is what
        // leaves the entry in the tree at zero rather than merely emptied.
        var a = Int(figures.node)
        while true {
            arena.logical[a] += sign * figures.logical
            arena.allocated[a] += sign * figures.allocated
            arena.fileCount[a] += Int32(sign) * figures.fileCount
            arena.addedLastMonth[a] += sign * figures.addedLastMonth
            arena.addedLastSixMonths[a] += sign * figures.addedLastSixMonths
            arena.addedSinceLastScan[a] += sign * figures.addedSinceLastScan
            if a == 0 { break }
            a = Int(arena.parent[a])
        }

        // adjustSubtree covers the descendants' per-kind figures but not the
        // entry itself, which only counts when it is a file.
        if !figures.isDirectory {
            let k = Int(figures.kind)
            kindBytesStore[k] += sign * figures.allocated
            kindLogicalStore[k] += sign * figures.logical
            kindCountStore[k] += Int(sign)
            kindMonthStore[k] += sign * figures.addedLastMonth
            kindSixStore[k] += sign * figures.addedLastSixMonths
            kindScanStore[k] += sign * figures.addedSinceLastScan
            fileTotal += Int(sign)
        } else {
            directoryTotal += Int(sign)
        }
    }

    /// Takes a subtree back out of every figure that includes it.
    private func discardSubtree(under node: Int32) {
        adjustSubtree(under: node, sign: -1)

        // Release the hard-link claims made inside the subtree, or the files that
        // hold them would be treated as duplicates on the way back in and their
        // bytes would vanish.
        if options.countHardlinksOnce, !seenMultiLinkInodes.isEmpty {
            seenMultiLinkInodes = seenMultiLinkInodes.filter { !isDescendant($0.value, of: node) }
        }

        arena.childStart[Int(node)] = 0
        arena.childCount[Int(node)] = 0
        arena.publish()
    }

    /// Walks the parent chain, which is why orphaning a subtree is safe: parents
    /// stay correct for nodes nothing points at any more.
    private func isDescendant(_ node: Int32, of ancestor: Int32) -> Bool {
        if node == ancestor { return true }
        var current = node
        while current != 0 {
            current = arena.parent[Int(current)]
            if current == ancestor { return true }
        }
        return false
    }

    /// Runs the workers and the applier over everything reachable from `seed`.
    private func walk(seed: DirWork) {
        stopLock.lock()
        stopRequested = false
        stopLock.unlock()
        batches = BatchQueue()
        setStopProgress(false)
        let dirs = DirQueue(seed: seed)
        stopLock.lock()
        let alreadyStopped = stopRequested
        liveDirs = dirs
        stopLock.unlock()
        // stop() may have been called before the thread got this far.
        if alreadyStopped { dirs.cancel() }
        let progressThread = startProgressReporter()

        stopLock.lock()
        walkEpoch &+= 1
        abandonedWorkers = 0
        stopLock.unlock()
        incompleteCloudRoots.removeAll()
        abandonedNodesLock.lock()
        abandonedNodes.removeAll()
        abandonedNodesLock.unlock()

        progressLock.lock()
        workerNode = Array(repeating: -1, count: options.workers)
        workerSince = Array(repeating: 0, count: options.workers)
        workerInCloud = Array(repeating: false, count: options.workers)
        progressLock.unlock()

        var workers: [Thread] = []
        let queue = batches
        let epoch = currentEpoch
        for index in 0..<options.workers {
            let t = Thread { [weak self] in
                self?.workerLoop(
                    dirs, index: index, batches: queue, epoch: epoch, seed: seed)
            }
            t.stackSize = 1 << 20
            t.start()
            workers.append(t)
        }
        // Signalled by the applier as it returns. `Thread.isFinished` says the
        // same thing but is only a poll: it carries no ordering, so the totals
        // the applier owns would be read here with nothing guaranteeing they had
        // arrived. Waiting on this is what makes them safe to read below, and
        // the thread sanitizer agrees.
        let applierDone = DispatchSemaphore(value: 0)
        let applier = Thread { [weak self] in
            self?.applierLoop(dirs)
            applierDone.signal()
        }
        applier.stackSize = 1 << 20
        applier.start()

        // How long the condition has held, not how long the worker has been in
        // its directory. Those are different numbers and using the second one
        // meant a single poll could authorise abandoning, which is the whole
        // safeguard gone: the condition flickers true for a moment every time
        // the applier empties the stack before pushing a batch's subdirectories,
        // and cancelling in that instant drops the local subtrees it was about
        // to queue. Requiring it to hold across every poll for two seconds is
        // what makes it a statement about the scan rather than about one sample.
        var heldSince: UInt64?
        while workers.contains(where: { !$0.isFinished }) {
            if onlyWaitingOnProviders(dirs: dirs) {
                let now = DispatchTime.now().uptimeNanoseconds
                heldSince = heldSince ?? now
                if Self.heldLongEnough(
                    since: heldSince, now: now, limit: Self.abandonAfterSeconds)
                {
                    abandon(dirs: dirs)
                    break
                }
            } else {
                heldSince = nil
            }
            usleep(2000)
        }
        batches.close()
        applierDone.wait()

        setStopProgress(true)
        progressThread?.cancel()
        arena.publish()
        stopLock.lock()
        liveDirs = nil
        stopLock.unlock()
    }

    private func makeResult(started: DispatchTime) -> ScanResult {
        stopLock.lock()
        let wasStopped = stopRequested
        let leftBehind = abandonedWorkers
        stopLock.unlock()

        var result = ScanResult(arena: arena)
        result.files = fileTotal
        result.directories = directoryTotal
        result.errors = errorTotal
        result.syscalls = syscallTotal
        result.entriesWithTimestamp = timestampedTotal
        result.sinceLastScanBaseline = options.cutoffSinceLastScan.map {
            Date(timeIntervalSince1970: Double($0))
        }
        result.hardlinkDuplicates = hardlinkDupes
        result.hardlinkDuplicateBytes = hardlinkDupBytes
        // Under the locks that own them. These used to be read bare, which was
        // safe only while every worker had certainly finished — and abandoning
        // one is precisely the case where that is no longer true. The thread
        // sanitizer reports them as races either way.
        stageLock.lock()
        result.secBuildPath = Double(nsBuildPath) / 1e9
        result.secOpen = Double(nsOpen) / 1e9
        result.secRead = Double(nsRead) / 1e9
        result.secPush = Double(nsPush) / 1e9
        result.secWorkerTotal = Double(nsWorkerTotal) / 1e9
        result.secApplier = Double(nsApplier) / 1e9
        result.secApplierTotal = Double(nsApplierTotal) / 1e9
        stageLock.unlock()
        let queueStats = batches.statistics
        result.queueMaxDepth = queueStats.maxDepth
        result.queueBlockedPushes = queueStats.blockedPushes
        result.elapsed =
            Double(DispatchTime.now().uptimeNanoseconds - started.uptimeNanoseconds) / 1e9
        result.stopped = wasStopped
        result.abandonedWorkers = leftBehind
        // Safe here and not before: the applier owns this set and has finished
        // by the time a result is made, so these are the last writes to it.
        abandonedNodesLock.lock()
        let leftInside = abandonedNodes
        abandonedNodesLock.unlock()
        for node in leftInside where Int(node) < arena.count {
            noteIncompleteCloudRoot(above: node)
        }
        result.cloudFoldersSkipped = incompleteCloudRoots.count
        return result
    }

    private func writeRootNode(named root: String) {
        let bytes = Array(root.utf8)
        _ = arena.reserveNodes(1)
        let off = arena.allocateName(bytes.count)
        for (j, b) in bytes.enumerated() { arena.names[off + j] = b }
        arena.parent[0] = 0
        arena.nameOffset[0] = Int32(off)
        arena.nameLength[0] = UInt16(min(bytes.count, Int(UInt16.max)))
        arena.kind[0] = FileKind.directory
        arena.flags[0] = NodeFlags.isDir
        arena.publish()
    }

    /// Records the first node to reach an inode. False means some earlier node
    /// already counted it — the claim is left with that one, so releasing a
    /// subtree's claims later cannot orphan an inode that is still counted.
    private func claimHardlink(_ inode: UInt64, by node: Int32) -> Bool {
        if seenMultiLinkInodes[inode] != nil { return false }
        seenMultiLinkInodes[inode] = node
        return true
    }

    // MARK: - Applier (single writer)

    private func applierLoop(_ dirs: DirQueue) {
        let start = DispatchTime.now().uptimeNanoseconds
        var busy: UInt64 = 0
        while let batch = batches.take() {
            let t0 = DispatchTime.now().uptimeNanoseconds
            apply(batch, dirs: dirs)
            busy += DispatchTime.now().uptimeNanoseconds - t0
            pool.recycle(batch)
        }
        stageLock.lock()
        nsApplier = busy
        nsApplierTotal = DispatchTime.now().uptimeNanoseconds - start
        stageLock.unlock()
    }

    private func apply(_ batch: ScanBatch, dirs: DirQueue) {
        errorTotal += batch.errors
        syscallTotal += batch.syscalls

        let n = batch.nodes.count
        let parentNode = Int(batch.parentNode)

        // The applier is the only writer, so a flag a worker discovered is set
        // here rather than there.
        if batch.isCloudRoot {
            arena.flags[parentNode] |= NodeFlags.isCloudRoot
        }
        if batch.skippedAsCloud {
            arena.flags[parentNode] |= NodeFlags.isCloudUnread
            noteIncompleteCloudRoot(above: batch.parentNode)
        }

        if n > 0 {
            let base = arena.reserveNodes(n)

            var files = 0
            var bytes: Int64 = 0
            var logicalBytes: Int64 = 0
            var bytesMonth: Int64 = 0
            var bytesSixMonths: Int64 = 0
            var bytesSinceScan: Int64 = 0
            let cutoffMonth = options.cutoffLastMonth
            let cutoffSix = options.cutoffLastSixMonths
            let cutoffScan = options.cutoffSinceLastScan

            for (i, pn) in batch.nodes.enumerated() {
                let idx = base + i
                var lg = pn.logical
                var al = pn.allocated
                var fc = pn.fileCount
                var fl = pn.flags

                // Hard-link suppression happens before anything is rolled up, so a
                // file reachable by several paths contributes its bytes once and
                // running totals are never transiently inflated.
                if options.countHardlinksOnce, pn.multiLinkInode != 0,
                    !claimHardlink(pn.multiLinkInode, by: Int32(idx))
                {
                    hardlinkDupes += 1
                    hardlinkDupBytes += al
                    lg = 0
                    al = 0
                    fc = 0
                    fl |= NodeFlags.isHardlinkDup
                }

                let len = Int(pn.nameLength)
                let off = arena.allocateName(len)
                let src = Int(pn.nameOffset)
                batch.names.withUnsafeBytes { raw in
                    arena.writeName(raw.baseAddress! + src, count: len, at: off)
                }

                arena.parent[idx] = batch.parentNode
                arena.nameOffset[idx] = Int32(off)
                arena.nameLength[idx] = pn.nameLength
                arena.kind[idx] = pn.kind
                arena.flags[idx] = fl
                arena.logical[idx] = lg
                arena.allocated[idx] = al
                arena.fileCount[idx] = fc
                arena.childStart[idx] = 0
                arena.childCount[idx] = 0
                arena.addedTime[idx] = pn.addedTime

                // Bucket this entry's own bytes, then roll the batch totals up.
                // Suppressed hard links already have al == 0, so they contribute
                // nothing here either.
                var inMonth: Int64 = 0
                var inSix: Int64 = 0
                var inScan: Int64 = 0
                if pn.addedTime > 0 {
                    timestampedTotal += 1
                    if pn.addedTime >= cutoffSix {
                        inSix = al
                        if pn.addedTime >= cutoffMonth { inMonth = al }
                    }
                    if let cutoffScan, pn.addedTime >= cutoffScan { inScan = al }
                }
                arena.addedLastMonth[idx] = inMonth
                arena.addedLastSixMonths[idx] = inSix
                arena.addedSinceLastScan[idx] = inScan
                bytesMonth += inMonth
                bytesSixMonths += inSix
                bytesSinceScan += inScan

                if fl & NodeFlags.isDir == 0 {
                    let k = Int(pn.kind)
                    kindBytesStore[k] += al
                    kindLogicalStore[k] += lg
                    kindCountStore[k] += 1
                    kindMonthStore[k] += inMonth
                    kindSixStore[k] += inSix
                    kindScanStore[k] += inScan
                }

                files += Int(fc)
                bytes += al
                logicalBytes += lg
            }

            fileTotal += files
            directoryTotal += batch.subdirIndices.count

            // Children of a directory are one contiguous run, which is what makes
            // the outline view's child lookup O(1) with no auxiliary structure.
            arena.childStart[parentNode] = Int32(base)
            arena.childCount[parentNode] = Int32(n)

            // Roll this directory's contents into every ancestor. Single writer, so
            // no read-modify-write race is possible.
            var a = parentNode
            while true {
                arena.logical[a] += logicalBytes
                arena.allocated[a] += bytes
                arena.fileCount[a] += Int32(files)
                arena.addedLastMonth[a] += bytesMonth
                arena.addedLastSixMonths[a] += bytesSixMonths
                arena.addedSinceLastScan[a] += bytesSinceScan
                if a == 0 { break }
                a = Int(arena.parent[a])
            }

            arena.publish()

            var children: [DirWork] = []
            children.reserveCapacity(batch.subdirIndices.count)
            for local in batch.subdirIndices {
                children.append(Int32(base + Int(local)))
            }
            dirs.push(children)
        }

        dirs.completed()
    }

    // MARK: - Progress

    private func startProgressReporter() -> Thread? {
        guard let report = options.progress else { return nil }
        let t = Thread { [weak self] in
            while let self, !self.shouldStopProgress {
                // Lock-free: readers only need the published count and the arena.
                let nodes = self.arena.count

                // The worker that has been on the same directory longest. On a
                // healthy tree that is a few milliseconds and means nothing; on a
                // cloud-backed one it is the folder everything is waiting for.
                self.progressLock.lock()
                var oldest: (node: Int32, since: UInt64)?
                for (i, node) in self.workerNode.enumerated() where node >= 0 {
                    let since = self.workerSince[i]
                    if oldest == nil || since < oldest!.since {
                        oldest = (node, since)
                    }
                }
                self.progressLock.unlock()

                var path: String?
                var stalled = false
                var inCloud = false
                if let oldest, Int(oldest.node) < nodes {
                    let waited =
                        Double(DispatchTime.now().uptimeNanoseconds &- oldest.since) / 1e9
                    stalled = waited > 1
                    path = self.arena.path(Int(oldest.node))
                    if stalled, let path {
                        inCloud = self.cloudRoots.contains(Array(path.utf8)[...])
                    }
                }

                report(
                    ScanProgress(
                        files: Int(self.arena.fileCount[0]),
                        directories: nodes - Int(self.arena.fileCount[0]),
                        nodes: nodes,
                        allocatedBytes: self.arena.allocated[0],
                        currentPath: path,
                        stalled: stalled,
                        stalledInCloudFolder: inCloud))
                usleep(100_000)
            }
        }
        t.start()
        return t
    }

    // MARK: - Workers (read-only, never touch the arena)

    /// How long every remaining worker must be stuck in a provider's folder
    /// before the walk stops waiting for them.
    ///
    /// Long enough that an ordinary slow directory is never mistaken for one,
    /// short enough that a person who has just asked to skip these does not sit
    /// watching a finished scan claim to be running.
    static let abandonAfterSeconds = 2.0

    /// The decision itself, over nothing but values.
    ///
    /// Kept apart from the scanner's state so it can be checked directly: the
    /// alternative was reaching into a live scanner's worker table from a test,
    /// which needed two extra methods, looser access, and left the table able to
    /// disagree with the number of workers actually running.
    ///
    /// `held` is one entry per worker that is holding a directory right now;
    /// workers waiting on the queue are not in it, because they are holding
    /// nothing and there is nothing of theirs to abandon.
    static func onlyWaitingOnProviders(
        skipping: Bool, drained: Bool, held: [Bool]
    ) -> Bool {
        guard skipping, drained, !held.isEmpty else { return false }
        // One worker on an ordinary local directory means the scan is going
        // somewhere, and cutting it off would drop whatever is under it.
        return held.allSatisfy { $0 }
    }

    /// Whether the condition has now held long enough to act on.
    ///
    /// Kept apart from the condition so it can be checked on its own: this is
    /// the part that was wrong. The wait used to be measured from the worker's
    /// arrival in its directory, which is already old by the time anyone looks,
    /// so a single poll could authorise abandoning a scan.
    static func heldLongEnough(since: UInt64?, now: UInt64, limit: Double) -> Bool {
        guard let since else { return false }
        return Double(now &- since) / 1e9 > limit
    }

    /// When the only thing left to wait for is a provider, or nil if anything
    /// else is still going on.
    ///
    /// Every condition has to hold at once. Skipping must be on, so this only
    /// ever happens to someone who asked for it. The queue must have nothing
    /// left to hand out, or an idle worker would pick it up and the scan would
    /// carry on legitimately. And every worker still holding a directory must be
    /// inside a provider's folder — one busy with a slow local directory means
    /// the scan is making progress and its results would be truncated.
    private func onlyWaitingOnProviders(dirs: DirQueue) -> Bool {
        let skipping = isSkippingCloudFolders
        let drained = dirs.isDrained
        return Self.onlyWaitingOnProviders(
            skipping: skipping, drained: drained, held: heldDirectories().map(\.inCloud))
    }

    /// One entry per worker currently holding a directory, with the node it is
    /// inside. Workers waiting on the queue hold nothing and are not listed.
    private func heldDirectories() -> [(node: Int32, inCloud: Bool)] {
        progressLock.lock()
        defer { progressLock.unlock() }
        var held: [(node: Int32, inCloud: Bool)] = []
        for index in 0..<workerNode.count
        where workerNode[index] >= 0 && index < workerInCloud.count {
            held.append((workerNode[index], workerInCloud[index]))
        }
        return held
    }

    /// Leaves the stuck workers to finish on their own time.
    ///
    /// Nothing is killed — a thread inside a syscall cannot be — so this only
    /// stops waiting. Moving the epoch on is what makes that safe: those workers
    /// will find their walk is over and drop what they are carrying instead of
    /// pushing it into whatever runs next.
    func abandon(dirs: DirQueue) {
        // Only the workers actually holding a provider's directory. Threads
        // waiting on the queue are unfinished too, and counting those reported
        // twenty folders left unread where thirteen were.
        let stuck = heldDirectories().filter(\.inCloud)
        let left = stuck.count
        // Where they were, so the provider each belongs to can be named once the
        // applier has stopped. Their own directories never reach it, so without
        // this nothing records that those subtrees are missing at all.
        abandonedNodesLock.lock()
        abandonedNodes = stuck.map(\.node)
        abandonedNodesLock.unlock()
        stopLock.lock()
        walkEpoch &+= 1
        abandonedWorkers = left
        stopLock.unlock()
        dirs.cancel()
    }

    var currentEpoch: Int {
        stopLock.lock()
        defer { stopLock.unlock() }
        return walkEpoch
    }

    /// False once the walk that started this worker has ended.
    func isCurrent(_ epoch: Int) -> Bool {
        stopLock.lock()
        defer { stopLock.unlock() }
        return epoch == walkEpoch
    }

    /// Records the provider this unread directory belongs to.
    ///
    /// Walks up to the nearest ancestor marked as a provider root, which is a
    /// few steps and only happens for directories being skipped. A directory
    /// whose root is not marked — it can happen if the walk was seeded inside
    /// one — counts as its own, which is better than not counting it.
    func noteIncompleteCloudRoot(above node: Int32) {
        var current = node
        while true {
            if arena.flags[Int(current)] & NodeFlags.isCloudRoot != 0 {
                incompleteCloudRoots.insert(current)
                return
            }
            let parent = arena.parent[Int(current)]
            if parent == current || current == 0 { break }
            current = parent
        }
        incompleteCloudRoots.insert(node)
    }

    /// Whether this directory belongs to a sync provider, and whether to leave it
    /// unread.
    ///
    /// `pathBuffer` is NUL-terminated, as the open below wants it, so the trailing
    /// byte is dropped before any comparison against a root.
    private func classifyCloud(
        path pathBuffer: [UInt8], isScanRoot: Bool
    ) -> (isRoot: Bool, inCloud: Bool, skip: Bool) {
        let path = pathBuffer.dropLast()  // the NUL the open() below wants
        // Already known counts: a root seeded into the registry, or found by
        // attribute earlier in this scan, does not stop being one because the
        // name filter would not have picked it out.
        var isRoot = cloudRoots.isRoot(path)
        if !isRoot, isScanRoot || CloudFolders.mayBeProviderRoot(path: path) {
            isRoot = pathBuffer.withUnsafeBufferPointer { buf -> Bool in
                buf.baseAddress!.withMemoryRebound(to: CChar.self, capacity: buf.count) {
                    CloudFolders.isProviderRoot(cPath: $0)
                }
            }
        }
        if isRoot {
            cloudRoots.add(Array(path))
        }
        // Under a root as well as at it, and worked out whether or not skipping
        // is on at this instant: the offer to skip arrives mid-scan, so a worker
        // already inside one of these directories has to know it is in one
        // before it can be told to stop.
        let inCloud = isRoot || cloudRoots.contains(path)
        return (isRoot, inCloud, inCloud && isSkippingCloudFolders)
    }

    /// Records where a worker is, for the progress reporter. Once per directory,
    /// not once per entry.
    private func noteWorker(_ index: Int, at node: Int32) {
        progressLock.lock()
        if index < workerNode.count {
            workerNode[index] = node
            workerSince[index] = DispatchTime.now().uptimeNanoseconds
        }
        progressLock.unlock()
    }

    /// This worker is between directories, holding nothing.
    ///
    /// Takes the epoch because the push before it can block on a full queue: the
    /// guard could pass, the walk end, a new one resize this table, and the
    /// clear then land on a live worker of that new walk and make it look idle.
    private func clearWorker(_ index: Int, epoch: Int) {
        stopLock.lock()
        let current = epoch == walkEpoch
        stopLock.unlock()
        guard current else { return }
        progressLock.lock()
        if index < workerNode.count {
            workerNode[index] = -1
            workerInCloud[index] = false
        }
        progressLock.unlock()
    }

    private func noteWorkerCloud(_ index: Int, _ inCloud: Bool) {
        progressLock.lock()
        if index < workerInCloud.count { workerInCloud[index] = inCloud }
        progressLock.unlock()
    }

    /// One I/O worker.
    ///
    /// The batch queue arrives as a parameter rather than being read off `self`,
    /// which is what makes abandoning a worker safe: a walk replaces the
    /// property, so a worker that woke late and pushed through it would be
    /// handing a directory from the abandoned tree to the scan running now.
    /// Holding its own queue, the worst it can do is push into one nobody is
    /// draining.
    private func workerLoop(
        _ dirs: DirQueue, index: Int, batches: BatchQueue, epoch: Int, seed: DirWork
    ) {
        let bufSize = options.bulkBufferSize
        let buffer = UnsafeMutableRawPointer.allocate(byteCount: bufSize, alignment: 16)
        defer { buffer.deallocate() }
        // Reused for the lifetime of the worker: walking a tree allocates nothing.
        var pathBuffer = [UInt8]()
        pathBuffer.reserveCapacity(1024)
        var chain = [Int32]()
        chain.reserveCapacity(64)

        var tBuild: UInt64 = 0, tOpen: UInt64 = 0, tRead: UInt64 = 0, tPush: UInt64 = 0
        let workerStart = DispatchTime.now().uptimeNanoseconds

        while let node = dirs.take() {
            noteWorker(index, at: node)
            let batch = pool.obtain()
            batch.reset(parentNode: node)

            var t = DispatchTime.now().uptimeNanoseconds
            buildPath(of: node, into: &pathBuffer, chain: &chain)
            let t1 = DispatchTime.now().uptimeNanoseconds
            tBuild += t1 - t
            t = t1

            // Asked before opening, and answered by an extended attribute rather
            // than by reading anything: opening a provider's directory is what
            // costs seconds, and touching a placeholder can make it fetch the
            // file. The path is already in hand, so this adds no string work.
            let cloud = classifyCloud(path: pathBuffer, isScanRoot: node == seed)
            noteWorkerCloud(index, cloud.inCloud)
            batch.isCloudRoot = cloud.isRoot
            if cloud.skip {
                batch.skippedAsCloud = true
                guard isCurrent(epoch) else { return }
                batches.push(batch)
                clearWorker(index, epoch: epoch)
                continue
            }

            let fd = pathBuffer.withUnsafeBufferPointer { buf -> Int32 in
                buf.baseAddress!.withMemoryRebound(to: CChar.self, capacity: buf.count) {
                    open($0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                }
            }
            let t2 = DispatchTime.now().uptimeNanoseconds
            tOpen += t2 - t
            t = t2

            guard fd >= 0 else {
                batch.errors += 1
                guard isCurrent(epoch) else { return }
                batches.push(batch)   // still enqueued, so the directory is accounted for
                clearWorker(index, epoch: epoch)
                continue
            }

            switch options.backend {
            case .bulk:
                readDirectoryBulk(
                    fd: fd, buffer: buffer, bufSize: bufSize, batch: batch,
                    inCloudFolder: cloud.inCloud)
            case .stat:
                readDirectoryStat(fd: fd, batch: batch)
            }
            close(fd)
            let t3 = DispatchTime.now().uptimeNanoseconds
            tRead += t3 - t

            // Nothing below this point may touch shared state if the walk has
            // ended: the arena it would describe belongs to a scan that is over.
            guard isCurrent(epoch) else { return }
            batches.push(batch)
            clearWorker(index, epoch: epoch)
            tPush += DispatchTime.now().uptimeNanoseconds - t3
        }

        let total = DispatchTime.now().uptimeNanoseconds - workerStart
        guard isCurrent(epoch) else { return }
        stageLock.lock()
        nsBuildPath += tBuild
        nsOpen += tOpen
        nsRead += tRead
        nsPush += tPush
        nsWorkerTotal += total
        stageLock.unlock()
    }

    /// Rebuilds an absolute path from the arena into a caller-owned buffer.
    ///
    /// Safe without synchronisation: the applier publishes a node before it queues
    /// that node's children, so every name on the ancestor chain is already visible
    /// and immutable by the time a worker asks for it.
    private func buildPath(of node: Int32, into buf: inout [UInt8], chain: inout [Int32]) {
        chain.removeAll(keepingCapacity: true)
        var cur = Int(node)
        while cur > 0 {
            chain.append(Int32(cur))
            cur = Int(arena.parent[cur])
        }
        buf.removeAll(keepingCapacity: true)
        let rootName = arena.nameBytes(0)
        buf.append(contentsOf: rootName)
        for n in chain.reversed() {
            if buf.last != 0x2F { buf.append(0x2F) }
            buf.append(contentsOf: arena.nameBytes(Int(n)))
        }
        buf.append(0)
    }

    /// What a file's flags and logical size become, given what the filesystem
    /// says about it.
    ///
    /// A placeholder's allocated size is already zero, so the figure the app
    /// shows by default needs no help. Its logical size is the size on someone
    /// else's server, and adding that up makes a folder look like it is holding
    /// gigabytes that no disk anywhere is holding — so it does not count either.
    ///
    /// A value function because `SF_DATALESS` cannot be set with `chflags`:
    /// there is no way to build a file a test could scan, so the decision is
    /// checked directly instead of being taken on trust.
    static func fileEntry(stFlags: UInt32, logical: Int64) -> (flags: UInt8, logical: Int64) {
        guard CloudFolders.isDataless(stFlags) else { return (0, logical) }
        return (NodeFlags.isCloudPlaceholder, 0)
    }

    /// "Added to the folder" where the filesystem knows it, birth time otherwise.
    @inline(__always)
    private func entryTimestamp(_ e: BulkEntry) -> UInt32 {
        let t = e.addedTime > 0 ? e.addedTime : e.createTime
        return t > 0 ? UInt32(truncatingIfNeeded: t) : 0
    }

    private func readDirectoryBulk(
        fd: Int32, buffer: UnsafeMutableRawPointer, bufSize: Int, batch: ScanBatch,
        inCloudFolder: Bool = false
    ) {
        var attrs = makeAttrList(options.attributeProfile)
        while true {
            // Asked once per batch of entries rather than once per directory. A
            // directory opened before the user asked to skip these would
            // otherwise go on pulling entries out of the provider a syscall at a
            // time, for as long as that took, and the answer would not arrive
            // until it finished.
            if inCloudFolder && isSkippingCloudFolders {
                batch.skippedAsCloud = true
                return
            }
            let n = withUnsafeMutablePointer(to: &attrs) { alist in
                getattrlistbulk(fd, alist, buffer, bufSize, 0)
            }
            batch.syscalls += 1
            if n < 0 { batch.errors += 1; return }
            if n == 0 { return }

            var p = UnsafeRawPointer(buffer)
            for _ in 0..<n {
                let (e, len) = decodeEntry(p)
                p += len
                if e.error != 0 || e.nameLen == 0 { batch.errors += 1; continue }
                if isDotOrDotDot(e.nameStart, e.nameLen) { continue }

                switch e.objType {
                case VType.dir:
                    if e.isMountPoint && !options.crossMountPoints {
                        batch.append(
                            name: e.nameStart, nameLen: e.nameLen, kind: FileKind.directory,
                            flags: NodeFlags.isDir | NodeFlags.isMountPoint,
                            logical: 0, allocated: 0, isFile: false)
                        continue
                    }
                    batch.subdirIndices.append(Int32(batch.nodes.count))
                    batch.append(
                        name: e.nameStart, nameLen: e.nameLen, kind: FileKind.directory,
                        flags: NodeFlags.isDir, logical: e.logicalSize,
                        allocated: e.allocSize, isFile: false, addedTime: entryTimestamp(e))

                case VType.lnk:
                    batch.append(
                        name: e.nameStart, nameLen: e.nameLen, kind: FileKind.symlink,
                        flags: NodeFlags.isSymlink, logical: e.logicalSize,
                        allocated: e.allocSize, isFile: true, addedTime: entryTimestamp(e))

                case VType.reg:
                    let file = Self.fileEntry(stFlags: e.stFlags, logical: e.logicalSize)
                    batch.append(
                        name: e.nameStart, nameLen: e.nameLen,
                        kind: FileKind.classify(name: e.nameStart, length: e.nameLen),
                        flags: file.flags,
                        logical: file.logical,
                        allocated: e.allocSize,
                        isFile: true, multiLinkInode: e.linkCount > 1 ? e.fileID : 0,
                        addedTime: entryTimestamp(e))

                default:
                    batch.append(
                        name: e.nameStart, nameLen: e.nameLen, kind: FileKind.unknown,
                        flags: 0, logical: 0, allocated: 0, isFile: true)
                }
            }
        }
    }

    @inline(__always)
    private func birthStamp(_ st: stat) -> UInt32 {
        let t = st.st_birthtimespec.tv_sec
        return t > 0 ? UInt32(truncatingIfNeeded: t) : 0
    }

    private func readDirectoryStat(fd: Int32, batch: ScanBatch) {
        let dupFd = dup(fd)
        guard dupFd >= 0, let dir = fdopendir(dupFd) else {
            if dupFd >= 0 { close(dupFd) }
            batch.errors += 1
            return
        }
        defer { closedir(dir) }

        while let entPtr = readdir(dir) {
            batch.syscalls += 1
            var ent = entPtr.pointee
            let nameLen = Int(ent.d_namlen)
            let isDot: Bool = withUnsafeBytes(of: &ent.d_name) { raw in
                isDotOrDotDot(raw.baseAddress!, nameLen)
            }
            if isDot { continue }

            var st = stat()
            let ok: Bool = withUnsafeBytes(of: &ent.d_name) { raw in
                fstatat(
                    fd, raw.baseAddress!.assumingMemoryBound(to: CChar.self), &st,
                    AT_SYMLINK_NOFOLLOW) == 0
            }
            batch.syscalls += 1
            guard ok else { batch.errors += 1; continue }

            let mode = st.st_mode & S_IFMT
            let logical = Int64(st.st_size)
            let allocated = Int64(st.st_blocks) * 512

            withUnsafeBytes(of: &ent.d_name) { raw in
                let namePtr = raw.baseAddress!
                if mode == S_IFDIR {
                    if !options.crossMountPoints && st.st_dev != rootDev {
                        batch.append(
                            name: namePtr, nameLen: nameLen, kind: FileKind.directory,
                            flags: NodeFlags.isDir | NodeFlags.isMountPoint,
                            logical: 0, allocated: 0, isFile: false)
                        return
                    }
                    batch.subdirIndices.append(Int32(batch.nodes.count))
                    batch.append(
                        name: namePtr, nameLen: nameLen, kind: FileKind.directory,
                        flags: NodeFlags.isDir, logical: logical, allocated: allocated,
                        isFile: false, addedTime: birthStamp(st))
                } else if mode == S_IFLNK {
                    batch.append(
                        name: namePtr, nameLen: nameLen, kind: FileKind.symlink,
                        flags: NodeFlags.isSymlink, logical: logical, allocated: allocated,
                        isFile: true, addedTime: birthStamp(st))
                } else if mode == S_IFREG {
                    batch.append(
                        name: namePtr, nameLen: nameLen,
                        kind: FileKind.classify(name: namePtr, length: nameLen),
                        flags: 0, logical: logical, allocated: allocated, isFile: true,
                        multiLinkInode: st.st_nlink > 1 ? UInt64(st.st_ino) : 0,
                        addedTime: birthStamp(st))
                } else {
                    batch.append(
                        name: namePtr, nameLen: nameLen, kind: FileKind.unknown,
                        flags: 0, logical: 0, allocated: 0, isFile: true)
                }
            }
        }
    }
}

@inline(__always)
func isDotOrDotDot(_ p: UnsafeRawPointer, _ len: Int) -> Bool {
    let b = p.assumingMemoryBound(to: UInt8.self)
    if len == 1 { return b[0] == 0x2E }
    if len == 2 { return b[0] == 0x2E && b[1] == 0x2E }
    return false
}
