#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#include "batch.h"

namespace diskscan {

/// A directory waiting to be walked, identified purely by node index. The path is
/// rebuilt on demand from the arena, so queueing a directory allocates nothing.
using DirWork = int32_t;

/// Pending-directory pool shared by the scan workers.
///
/// Termination is the subtle part, because a directory is not finished when its
/// worker stops reading it — it is finished when the *applier* has recorded its
/// entries and pushed its subdirectories. `pending_` counts directories that have
/// been discovered but not yet applied, and the applier is what decrements it.
class DirQueue {
public:
    explicit DirQueue(DirWork seed) : stack_{seed}, pending_(1) {}

    /// Called by workers. Returns false once the whole tree has been applied.
    bool take(DirWork* out);

    /// Called by the applier, before `completed()` for the same directory, so the
    /// pending count can never transiently hit zero while children still exist.
    void push(const std::vector<DirWork>& items);

    /// Called by the applier once a directory's batch has been fully recorded.
    void completed();

    void cancel();

private:
    std::vector<DirWork> stack_;
    int64_t pending_ = 0;
    bool done_ = false;
    std::mutex mutex_;
    std::condition_variable cond_;
};

/// Bounded multi-producer, single-consumer queue of scan batches.
///
/// Bounded on purpose: workers can outrun the applier, and an unbounded queue
/// would let batches accumulate without limit on a tree with many small files.
/// Hitting the bound blocks a producer, which is backpressure rather than
/// contention — at the measured rates it should essentially never fire.
class BatchQueue {
public:
    explicit BatchQueue(size_t capacity = 1024) : capacity_(capacity) {}

    void push(ScanBatch* batch);
    /// Consumer side. Returns nullptr once closed and drained.
    ScanBatch* take();
    void close();

    /// High-water mark, for checking whether backpressure ever actually engaged.
    size_t maxDepth() const { return maxDepth_; }
    size_t blockedPushes() const { return blockedPushes_; }

private:
    std::vector<ScanBatch*> items_;
    size_t head_ = 0;
    size_t capacity_;
    bool closed_ = false;
    size_t maxDepth_ = 0;
    size_t blockedPushes_ = 0;
    std::mutex mutex_;
    std::condition_variable cond_;
};

}  // namespace diskscan
