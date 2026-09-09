import Darwin
import Foundation

/// One entry discovered while walking a directory, before it has been given a
/// node index. Array-of-structs here (rather than the arena's struct-of-arrays)
/// keeps a batch to two allocations instead of ten; the applier scatters the
/// fields into the arena as it appends.
struct PendingNode {
    var nameOffset: Int32   // into ScanBatch.names
    var nameLength: UInt16
    var kind: Int16
    var flags: UInt8
    var logical: Int64
    var allocated: Int64
    var fileCount: Int32
    /// Inode, but only for files with more than one link; 0 otherwise.
    var multiLinkInode: UInt64
    /// Epoch seconds this entry was added to its folder.
    var addedTime: UInt32
}

/// The unit handed from a scan worker to the applier: everything discovered in
/// one directory, plus the prebuilt child paths so the applier never touches the
/// file system or does string work.
final class ScanBatch {
    /// Node index of the directory these entries belong to.
    var parentNode: Int32 = 0
    var nodes: [PendingNode] = []
    var names: [UInt8] = []
    /// Indices into `nodes` that are directories needing their own walk. Paths are
    /// not carried: a worker rebuilds them from the arena, which costs no
    /// allocation at all.
    var subdirIndices: [Int32] = []
    var errors = 0
    var syscalls = 0
    /// This directory is the top of a sync provider's tree. Set by the worker
    /// that found it, acted on by the applier, which is the only thread allowed
    /// to write into the arena.
    var isCloudRoot = false
    /// Its contents were not read at all, because they belong to a provider and
    /// this scan was told to leave those alone.
    var skippedAsCloud = false

    func reset(parentNode: Int32) {
        self.parentNode = parentNode
        nodes.removeAll(keepingCapacity: true)
        names.removeAll(keepingCapacity: true)
        subdirIndices.removeAll(keepingCapacity: true)
        errors = 0
        syscalls = 0
        isCloudRoot = false
        skippedAsCloud = false
    }

    @inline(__always)
    func append(
        name: UnsafeRawPointer, nameLen: Int, kind k: Int16, flags f: UInt8,
        logical lg: Int64, allocated al: Int64, isFile: Bool, multiLinkInode: UInt64 = 0,
        addedTime: UInt32 = 0
    ) {
        let off = Int32(names.count)
        names.append(contentsOf: UnsafeRawBufferPointer(start: name, count: nameLen))
        nodes.append(
            PendingNode(
                nameOffset: off, nameLength: UInt16(min(nameLen, Int(UInt16.max))),
                kind: k, flags: f, logical: lg, allocated: al,
                fileCount: isFile ? 1 : 0, multiLinkInode: multiLinkInode,
                addedTime: addedTime))
    }
}

/// Recycles batches between the applier and the workers.
///
/// Without this, each worker would allocate fresh arrays per directory — roughly
/// a million allocations on a large tree — and, worse, handing a batch to the
/// queue while the worker still held a reference would trigger copy-on-write.
/// Ownership passes strictly: worker → queue → applier → pool → worker.
final class BatchPool {
    private var free: [ScanBatch] = []
    private let lock = NSLock()

    func obtain() -> ScanBatch {
        lock.lock()
        let b = free.popLast()
        lock.unlock()
        return b ?? ScanBatch()
    }

    func recycle(_ b: ScanBatch) {
        lock.lock()
        if free.count < 256 { free.append(b) }
        lock.unlock()
    }
}
