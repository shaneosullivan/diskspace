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
    /// Four is the measured knee on Apple-silicon NVMe: 4 workers beat both 2 and
    /// 8 on a 3.8M-entry tree. Past ~4, concurrent metadata reads interfere at the
    /// device level — it is an I/O limit, not a synchronisation one.
    public var workers: Int = max(1, min(4, ProcessInfo.processInfo.activeProcessorCount - 2))
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
    /// Nil when this root has never been scanned before, so the UI can say
    /// "no baseline yet" instead of showing a misleading zero.
    public var sinceLastScanBaseline: Date?
    /// How many entries reported a usable added/creation time, for confidence in
    /// the recency figures.
    public var entriesWithTimestamp = 0
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

    private let batches = BatchQueue()
    private let pool = BatchPool()

    // Applier-owned state — no synchronisation needed, single writer.
    private var seenMultiLinkInodes = Set<UInt64>()
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
    private let kindMonthStore: UnsafeMutablePointer<Int64>
    private let kindSixStore: UnsafeMutablePointer<Int64>
    private let kindScanStore: UnsafeMutablePointer<Int64>
    private let kindCountStore: UnsafeMutablePointer<Int>
    private let kindSlots: Int

    private var rootDev: dev_t = 0
    // Where the time actually goes, in nanoseconds.
    private let stageLock = NSLock()
    private var nsBuildPath: UInt64 = 0
    private var nsOpen: UInt64 = 0
    private var nsRead: UInt64 = 0
    private var nsPush: UInt64 = 0
    private var nsWorkerTotal: UInt64 = 0
    private var nsApplier: UInt64 = 0
    private var nsApplierTotal: UInt64 = 0
    private var stopProgress = false

    public init(options: ScanOptions = ScanOptions()) {
        self.options = options
        FileKind.warmUp()
        kindSlots = FileKind.names.count
        kindBytesStore = .allocate(capacity: kindSlots)
        kindBytesStore.initialize(repeating: 0, count: kindSlots)
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
        kindMonthStore.deallocate()
        kindSixStore.deallocate()
        kindScanStore.deallocate()
        kindCountStore.deallocate()
    }

    /// Snapshot of the per-kind totals. Safe to call while scanning: the applier
    /// only ever increases these, so a reader may see a value slightly behind but
    /// never a partial one.
    public func kindTotals(metric: SizeMetric = .total)
        -> [(kind: Int16, bytes: Int64, count: Int)]
    {
        let source: UnsafeMutablePointer<Int64>
        switch metric {
        case .total: source = kindBytesStore
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

    public func scan(root: String) -> ScanResult {
        let started = DispatchTime.now()

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

        let dirs = DirQueue(seed: 0)
        let progressThread = startProgressReporter()

        var workers: [Thread] = []
        for _ in 0..<options.workers {
            let t = Thread { [weak self] in self?.workerLoop(dirs) }
            t.stackSize = 1 << 20
            t.start()
            workers.append(t)
        }
        let applier = Thread { [weak self] in self?.applierLoop(dirs) }
        applier.stackSize = 1 << 20
        applier.start()

        while workers.contains(where: { !$0.isFinished }) { usleep(2000) }
        batches.close()
        while !applier.isFinished { usleep(2000) }

        stopProgress = true
        progressThread?.cancel()
        arena.publish()

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
        result.secBuildPath = Double(nsBuildPath) / 1e9
        result.secOpen = Double(nsOpen) / 1e9
        result.secRead = Double(nsRead) / 1e9
        result.secPush = Double(nsPush) / 1e9
        result.secWorkerTotal = Double(nsWorkerTotal) / 1e9
        result.secApplier = Double(nsApplier) / 1e9
        result.secApplierTotal = Double(nsApplierTotal) / 1e9
        result.queueMaxDepth = batches.maxDepth
        result.queueBlockedPushes = batches.blockedPushes
        result.elapsed =
            Double(DispatchTime.now().uptimeNanoseconds - started.uptimeNanoseconds) / 1e9
        // Recorded only on a completed scan, so an aborted run cannot move the
        // baseline forward and hide changes from the next comparison.
        ScanHistory.record(root: root)
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
        nsApplier = busy
        nsApplierTotal = DispatchTime.now().uptimeNanoseconds - start
    }

    private func apply(_ batch: ScanBatch, dirs: DirQueue) {
        errorTotal += batch.errors
        syscallTotal += batch.syscalls

        let n = batch.nodes.count
        let parentNode = Int(batch.parentNode)

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
                    !seenMultiLinkInodes.insert(pn.multiLinkInode).inserted
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
            while let self, !self.stopProgress {
                // Lock-free: readers only need the published count and the arena.
                let nodes = self.arena.count
                report(
                    ScanProgress(
                        files: Int(self.arena.fileCount[0]),
                        directories: nodes - Int(self.arena.fileCount[0]),
                        nodes: nodes,
                        allocatedBytes: self.arena.allocated[0]))
                usleep(100_000)
            }
        }
        t.start()
        return t
    }

    // MARK: - Workers (read-only, never touch the arena)

    private func workerLoop(_ dirs: DirQueue) {
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
            let batch = pool.obtain()
            batch.reset(parentNode: node)

            var t = DispatchTime.now().uptimeNanoseconds
            buildPath(of: node, into: &pathBuffer, chain: &chain)
            let t1 = DispatchTime.now().uptimeNanoseconds
            tBuild += t1 - t
            t = t1

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
                batches.push(batch)   // still enqueued, so the directory is accounted for
                continue
            }

            switch options.backend {
            case .bulk:
                readDirectoryBulk(fd: fd, buffer: buffer, bufSize: bufSize, batch: batch)
            case .stat:
                readDirectoryStat(fd: fd, batch: batch)
            }
            close(fd)
            let t3 = DispatchTime.now().uptimeNanoseconds
            tRead += t3 - t

            batches.push(batch)
            tPush += DispatchTime.now().uptimeNanoseconds - t3
        }

        let total = DispatchTime.now().uptimeNanoseconds - workerStart
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

    /// "Added to the folder" where the filesystem knows it, birth time otherwise.
    @inline(__always)
    private func entryTimestamp(_ e: BulkEntry) -> UInt32 {
        let t = e.addedTime > 0 ? e.addedTime : e.createTime
        return t > 0 ? UInt32(truncatingIfNeeded: t) : 0
    }

    private func readDirectoryBulk(
        fd: Int32, buffer: UnsafeMutableRawPointer, bufSize: Int, batch: ScanBatch
    ) {
        var attrs = makeAttrList(options.attributeProfile)
        while true {
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
                    batch.append(
                        name: e.nameStart, nameLen: e.nameLen,
                        kind: FileKind.classify(name: e.nameStart, length: e.nameLen),
                        flags: 0, logical: e.logicalSize, allocated: e.allocSize,
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
