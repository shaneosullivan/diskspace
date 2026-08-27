#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "name_string.h"

namespace diskscan {

/// One entry discovered while walking a directory, before it has been given a
/// node index. Array-of-structs here (rather than the arena's struct-of-arrays)
/// keeps a batch to two allocations instead of fourteen; the applier scatters the
/// fields into the arena as it appends.
struct PendingNode {
    int32_t nameOffset;  // into ScanBatch::names
    uint16_t nameLength;
    int16_t kind;
    uint8_t flags;
    int64_t logical;
    int64_t allocated;
    int32_t fileCount;
    /// The NTFS file reference number, used to count a file reachable by several
    /// paths only once. Zero when hard-link accounting is switched off, or when
    /// the file system did not supply one.
    uint64_t fileId;
    /// Epoch seconds this entry was created.
    uint32_t addedTime;
};

/// The unit handed from a scan worker to the applier: everything discovered in
/// one directory. Paths are not carried — a worker rebuilds them from the arena,
/// which costs no allocation at all.
struct ScanBatch {
    /// Node index of the directory these entries belong to.
    int32_t parentNode = 0;
    std::vector<PendingNode> nodes;
    std::vector<NameChar> names;
    /// Indices into `nodes` that are directories needing their own walk.
    std::vector<int32_t> subdirIndices;
    int errors = 0;
    int64_t syscalls = 0;
    /// Set when the directory itself could not be opened, so the applier can mark
    /// the node rather than silently reporting it as empty.
    bool unreadable = false;

    void reset(int32_t parent) {
        parentNode = parent;
        nodes.clear();
        names.clear();
        subdirIndices.clear();
        errors = 0;
        syscalls = 0;
        unreadable = false;
    }

    inline void append(const NameChar* name, size_t nameLen, int16_t kind, uint8_t flags,
                       int64_t logical, int64_t allocated, bool isFile, uint64_t fileId,
                       uint32_t addedTime) {
        int32_t off = int32_t(names.size());
        names.insert(names.end(), name, name + nameLen);
        if (nameLen > 0xFFFF) nameLen = 0xFFFF;
        nodes.push_back(PendingNode{off, uint16_t(nameLen), kind, flags, logical, allocated,
                                    isFile ? 1 : 0, fileId, addedTime});
    }
};

/// Recycles batches between the applier and the workers.
///
/// Without this, each worker would allocate fresh vectors per directory —
/// roughly a million allocations on a large tree. Ownership passes strictly:
/// worker -> queue -> applier -> pool -> worker, so a batch is never touched by
/// two threads at once and its buffers keep their capacity.
class BatchPool {
public:
    ~BatchPool() {
        for (ScanBatch* b : free_) delete b;
    }

    ScanBatch* obtain() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_.empty()) return new ScanBatch();
        ScanBatch* b = free_.back();
        free_.pop_back();
        return b;
    }

    void recycle(ScanBatch* b) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_.size() < 256) {
            free_.push_back(b);
        } else {
            delete b;
        }
    }

private:
    std::vector<ScanBatch*> free_;
    std::mutex mutex_;
};

}  // namespace diskscan
