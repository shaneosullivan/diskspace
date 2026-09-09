import Darwin
import Synchronization

/// Fixed-address chunked storage.
///
/// The point of the chunking is that memory never moves. A Swift `Array` would
/// reallocate on growth, which is fatal here: the UI reads nodes concurrently with
/// the applier appending them, and a reallocation would pull the ground out from
/// under an in-flight read. Chunks are allocated once and never resized, and the
/// chunk *table* is preallocated at full size so it never moves either.
public final class ChunkedArray<T> {
    @usableFromInline let table: UnsafeMutablePointer<UnsafeMutablePointer<T>?>
    @usableFromInline let shift: Int
    @usableFromInline let mask: Int
    private let chunkCapacity: Int
    private let maxChunks: Int
    private let zero: T
    private var allocatedChunks = 0

    public init(shift: Int, maxChunks: Int, zero: T) {
        self.shift = shift
        self.chunkCapacity = 1 << shift
        self.mask = chunkCapacity - 1
        self.maxChunks = maxChunks
        self.zero = zero
        table = .allocate(capacity: maxChunks)
        table.initialize(repeating: nil, count: maxChunks)
    }

    deinit {
        for i in 0..<allocatedChunks {
            table[i]?.deinitialize(count: chunkCapacity)
            table[i]?.deallocate()
        }
        table.deinitialize(count: maxChunks)
        table.deallocate()
    }

    /// Writer only. Must be called before any write at `index`, and therefore
    /// before the index is published to readers.
    public func ensureCapacity(_ index: Int) {
        let needed = (index >> shift) + 1
        precondition(needed <= maxChunks, "ChunkedArray exhausted")
        while allocatedChunks < needed {
            let chunk = UnsafeMutablePointer<T>.allocate(capacity: chunkCapacity)
            chunk.initialize(repeating: zero, count: chunkCapacity)
            table[allocatedChunks] = chunk
            allocatedChunks += 1
        }
    }

    /// Direct pointer to element `i`. Valid for a contiguous run only as far as the
    /// end of its chunk, so callers must know the run does not straddle one.
    @inlinable
    public func pointer(to i: Int) -> UnsafeMutablePointer<T> {
        table[i >> shift].unsafelyUnwrapped + (i & mask)
    }

    @inlinable
    public subscript(i: Int) -> T {
        @inline(__always) get { table[i >> shift].unsafelyUnwrapped[i & mask] }
        @inline(__always) set {
            table[i >> shift].unsafelyUnwrapped[i & mask] = newValue
        }
    }
}

/// Which of the four rolled-up size figures to display.
public enum SizeMetric: Int, CaseIterable {
    case total
    case addedLastMonth
    case addedLastSixMonths
    case addedSinceLastScan

    public var title: String {
        switch self {
        case .total: return "Current Size"
        case .addedLastMonth: return "Added in Last Month"
        case .addedLastSixMonths: return "Added in Last 6 Months"
        case .addedSinceLastScan: return "Added Since Last Scan"
        }
    }
}

public enum NodeFlags {
    public static let isDir: UInt8 = 1 << 0
    public static let isSymlink: UInt8 = 1 << 1
    public static let isMountPoint: UInt8 = 1 << 2
    public static let isHardlinkDup: UInt8 = 1 << 3
    /// Taken out of the tree by `remove`: either waiting in the pending list or
    /// already deleted. Its figures are out of every total, and the outline hides
    /// it. The node itself stays, because a pending delete has to be reversible
    /// and restoring needs somewhere to put the figures back.
    ///
    /// 1 << 4 is skipped to keep these numbered as the C++ scanner numbers them.
    /// The two are separate implementations of the same thing, and a bit that
    /// means one thing here and another there is a trap for whoever compares them.
    public static let isRemoved: UInt8 = 1 << 5

    /// A file whose contents are not on this disk: a sync provider's
    /// placeholder. It allocates nothing, so it is already absent from the
    /// figures; the flag is what lets the app say so rather than leave someone
    /// wondering why a folder full of files weighs nothing.
    public static let isCloudPlaceholder: UInt8 = 1 << 6

    /// The top of a sync provider's tree. Its contents may be entirely
    /// elsewhere, and walking it can be slow enough to look like a hang, so this
    /// is what both the display and the skipping key off.
    public static let isCloudRoot: UInt8 = 1 << 7
}

/// Struct-of-arrays node storage with one writer and many lock-free readers.
///
/// Concurrency contract:
///  * Exactly one thread (the applier) appends nodes and mutates rolled-up totals.
///  * Readers observe `count` with acquiring order; every node below that index has
///    been fully written, and its memory address is stable for the arena's lifetime.
///  * Rolled-up totals of already-published nodes keep changing as the scan
///    proceeds. Readers may see a total that is momentarily behind, which is
///    correct for a live display. Reads are naturally atomic: these are aligned
///    64-bit loads.
public final class NodeArena {
    static let nodeShift = 16   // 65,536 nodes per chunk
    static let nameShift = 20   // 1 MB of names per chunk

    public let parent: ChunkedArray<Int32>
    public let nameOffset: ChunkedArray<Int32>
    public let nameLength: ChunkedArray<UInt16>
    public let kind: ChunkedArray<Int16>
    public let flags: ChunkedArray<UInt8>
    public let logical: ChunkedArray<Int64>
    public let allocated: ChunkedArray<Int64>
    public let fileCount: ChunkedArray<Int32>
    /// A directory's entries are appended in one contiguous run, so children are
    /// the index range [childStart, childStart + childCount). No child pointers,
    /// no per-node list — the outline view indexes straight into it.
    public let childStart: ChunkedArray<Int32>
    public let childCount: ChunkedArray<Int32>
    /// When this entry was added to its folder, in epoch seconds.
    public let addedTime: ChunkedArray<UInt32>
    /// Subtree bytes whose entries were added inside each window. Rolled up the
    /// same way as `allocated`, so a folder's figure covers its whole subtree.
    public let addedLastMonth: ChunkedArray<Int64>
    public let addedLastSixMonths: ChunkedArray<Int64>
    public let addedSinceLastScan: ChunkedArray<Int64>
    public let names: ChunkedArray<UInt8>

    private let published = Atomic<Int>(0)
    private var writeCount = 0
    private var nameCursor = 0

    public init(maxNodes: Int = 1 << 28, maxNameBytes: Int = 1 << 33) {
        let nodeChunks = (maxNodes >> Self.nodeShift) + 1
        let nameChunks = (maxNameBytes >> Self.nameShift) + 1
        parent = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        nameOffset = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        nameLength = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        kind = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        flags = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        logical = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        allocated = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        fileCount = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        childStart = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        childCount = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        addedTime = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        addedLastMonth = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        addedLastSixMonths = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        addedSinceLastScan = ChunkedArray(shift: Self.nodeShift, maxChunks: nodeChunks, zero: 0)
        names = ChunkedArray(shift: Self.nameShift, maxChunks: nameChunks, zero: 0)
    }

    /// Nodes safe for readers to touch.
    public var count: Int { published.load(ordering: .acquiring) }

    /// Writer-side count, including nodes written but not yet published.
    public var uncheckedCount: Int { writeCount }

    // MARK: - Writer

    /// Reserves `n` consecutive node slots and guarantees their backing memory.
    public func reserveNodes(_ n: Int) -> Int {
        let base = writeCount
        guard n > 0 else { return base }
        let last = base + n - 1
        parent.ensureCapacity(last)
        nameOffset.ensureCapacity(last)
        nameLength.ensureCapacity(last)
        kind.ensureCapacity(last)
        flags.ensureCapacity(last)
        logical.ensureCapacity(last)
        allocated.ensureCapacity(last)
        fileCount.ensureCapacity(last)
        childStart.ensureCapacity(last)
        childCount.ensureCapacity(last)
        addedTime.ensureCapacity(last)
        addedLastMonth.ensureCapacity(last)
        addedLastSixMonths.ensureCapacity(last)
        addedSinceLastScan.ensureCapacity(last)
        writeCount += n
        return base
    }

    /// Reserves `len` name bytes that do not straddle a chunk boundary, so a name
    /// is always one contiguous run.
    public func allocateName(_ len: Int) -> Int {
        let chunkSize = 1 << Self.nameShift
        precondition(len <= chunkSize, "name longer than a name chunk")
        var off = nameCursor
        if (off & (chunkSize - 1)) + len > chunkSize {
            off = (off + chunkSize) & ~(chunkSize - 1)
        }
        names.ensureCapacity(off + len - 1)
        nameCursor = off + len
        return off
    }

    /// Makes every node written so far visible to readers.
    public func publish() {
        published.store(writeCount, ordering: .releasing)
    }

    // MARK: - Reader

    /// Bulk-writes a name. `allocateName` guarantees the run stays inside one
    /// chunk, so this is a single memcpy rather than a per-byte chunked store.
    @inline(__always)
    public func writeName(_ src: UnsafeRawPointer, count: Int, at offset: Int) {
        guard count > 0 else { return }
        memcpy(names.pointer(to: offset), src, count)
    }

    /// Raw name bytes, contiguous by construction.
    @inline(__always)
    public func nameBytes(_ i: Int) -> UnsafeBufferPointer<UInt8> {
        let len = Int(nameLength[i])
        guard len > 0 else { return UnsafeBufferPointer(start: nil, count: 0) }
        return UnsafeBufferPointer(start: names.pointer(to: Int(nameOffset[i])), count: len)
    }

    public func name(_ i: Int) -> String {
        let len = Int(nameLength[i])
        guard len > 0 else { return "" }
        return String(decoding: nameBytes(i), as: UTF8.self)
    }

    public func path(_ i: Int) -> String {
        var parts: [String] = []
        var cur = i
        while cur > 0 {
            parts.append(name(cur))
            cur = Int(parent[cur])
        }
        parts.append(name(0))
        return parts.reversed().joined(separator: "/")
    }

    public func isDirectory(_ i: Int) -> Bool { flags[i] & NodeFlags.isDir != 0 }

    @inline(__always)
    public func size(_ metric: SizeMetric, _ i: Int) -> Int64 {
        switch metric {
        case .total: return allocated[i]
        case .addedLastMonth: return addedLastMonth[i]
        case .addedLastSixMonths: return addedLastSixMonths[i]
        case .addedSinceLastScan: return addedSinceLastScan[i]
        }
    }

    /// Children of `i` that are currently published.
    public func children(of i: Int) -> Range<Int> {
        let start = Int(childStart[i])
        let n = Int(childCount[i])
        guard n > 0 else { return 0..<0 }
        let visible = min(start + n, count)
        return start < visible ? start..<visible : 0..<0
    }

    public func approximateMemoryBytes() -> Int {
        writeCount * (4 + 4 + 2 + 2 + 1 + 8 + 8 + 4 + 4 + 4 + 4 + 8 + 8 + 8) + nameCursor
    }
}
