import Foundation

/// A directory waiting to be walked, identified purely by node index. The path is
/// rebuilt on demand from the arena, so queueing a directory allocates nothing.
typealias DirWork = Int32

/// Pending-directory pool shared by the scan workers.
///
/// Termination is the subtle part now that a directory is not finished when its
/// worker stops reading it — it is finished when the *applier* has recorded its
/// entries and pushed its subdirectories. `pending` counts directories that have
/// been discovered but not yet applied, and the applier is what decrements it.
final class DirQueue {
    private var stack: [DirWork] = []
    private var pending = 0
    private var done = false
    private let cond = NSCondition()
    /// Which worker is holding which directory, and whether that directory
    /// belongs to a sync provider.
    ///
    /// Kept here rather than beside the other per-worker state so that the
    /// question "is a provider's folder the only thing left?" can be answered
    /// under one lock. Assembled from several it was never true of any single
    /// instant: between reading the queue and reading the workers, the applier
    /// could hand out a local directory that the answer then discarded.
    private var holders: [Int: Bool] = [:]

    init(seed: DirWork) {
        stack.append(seed)
        pending = 1
    }

    /// Called by workers. Returns nil once the whole tree has been applied.
    func take() -> DirWork? {
        cond.lock()
        defer { cond.unlock() }
        while true {
            if done { return nil }
            if let w = stack.popLast() { return w }
            cond.wait()
        }
    }

    /// A worker is holding a directory, and now knows whether it is inside a
    /// provider's tree.
    func holding(_ worker: Int, inCloud: Bool) {
        cond.lock()
        holders[worker] = inCloud
        cond.unlock()
    }

    /// A worker has handed its directory to the applier and holds nothing.
    func released(_ worker: Int) {
        cond.lock()
        holders.removeValue(forKey: worker)
        cond.unlock()
    }

    /// True when every directory still outstanding is in a worker's hands and
    /// every one of those is a provider's.
    ///
    /// `pending` counts everything discovered and not yet applied — queued,
    /// held by a worker, and sitting in the batch queue waiting for the
    /// applier. Equal to the number of held directories, it says all three of
    /// those are the same set, so nothing is queued and nothing is waiting to
    /// be recorded. One worker on an ordinary local directory says no.
    func onlyProviderWorkOutstanding() -> Bool {
        cond.lock()
        defer { cond.unlock() }
        guard !holders.isEmpty, pending == holders.count else { return false }
        return holders.values.allSatisfy { $0 }
    }

    /// Called by the applier, before `completed()` for the same directory, so the
    /// pending count can never transiently hit zero while children still exist.
    func push(_ items: [DirWork]) {
        guard !items.isEmpty else { return }
        cond.lock()
        pending += items.count
        stack.append(contentsOf: items)
        cond.broadcast()
        cond.unlock()
    }

    /// Called by the applier once a directory's batch has been fully recorded.
    func completed() {
        cond.lock()
        pending -= 1
        if pending == 0 {
            done = true
            cond.broadcast()
        }
        cond.unlock()
    }

    func cancel() {
        cond.lock()
        done = true
        cond.broadcast()
        cond.unlock()
    }

    /// Nothing left to hand out. Directories already taken may still be in
    /// progress; this says only that a worker asking now would have to wait.
    var isDrained: Bool {
        cond.lock()
        defer { cond.unlock() }
        return stack.isEmpty
    }
}

/// Bounded multi-producer, single-consumer queue of scan batches.
///
/// Bounded on purpose: workers can outrun the applier, and an unbounded queue
/// would let batches accumulate without limit on a tree with many small files.
/// Hitting the bound blocks a producer, which is backpressure rather than
/// contention — at the measured rates it should essentially never fire.
final class BatchQueue {
    private var items: [ScanBatch] = []
    private var head = 0
    private var closed = false
    private let capacity: Int
    private let cond = NSCondition()

    /// High-water mark, for checking whether backpressure ever actually engaged.
    private var maxDepth = 0
    private var blockedPushes = 0

    /// Both counters, read together under the lock that owns them.
    ///
    /// A worker left behind may still be inside `push` when the result is put
    /// together, so reading these bare is a genuine race rather than a
    /// theoretical one.
    var statistics: (maxDepth: Int, blockedPushes: Int) {
        cond.lock()
        defer { cond.unlock() }
        return (maxDepth, blockedPushes)
    }

    init(capacity: Int = 1024) {
        self.capacity = capacity
    }

    func push(_ batch: ScanBatch) {
        cond.lock()
        while items.count - head >= capacity && !closed {
            blockedPushes += 1
            cond.wait()
        }
        items.append(batch)
        let depth = items.count - head
        if depth > maxDepth { maxDepth = depth }
        cond.signal()
        cond.unlock()
    }

    /// Consumer side. Returns nil once closed and drained.
    func take() -> ScanBatch? {
        cond.lock()
        defer { cond.unlock() }
        while true {
            if head < items.count {
                let b = items[head]
                items[head] = ScanBatch()  // release our reference promptly
                head += 1
                if head > 4096 {
                    items.removeFirst(head)
                    head = 0
                }
                cond.broadcast()
                return b
            }
            if closed { return nil }
            cond.wait()
        }
    }

    func close() {
        cond.lock()
        closed = true
        cond.broadcast()
        cond.unlock()
    }
}
