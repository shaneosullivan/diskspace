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
    private(set) var maxDepth = 0
    private(set) var blockedPushes = 0

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
