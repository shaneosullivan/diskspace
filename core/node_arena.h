#pragma once

#include <atomic>
#include <string>

#include "name_string.h"

#include "chunked_array.h"

namespace diskscan {

/// Which of the four rolled-up size figures to display.
enum class SizeMetric : int {
    Total = 0,
    AddedLastMonth = 1,
    AddedLastSixMonths = 2,
    AddedSinceLastScan = 3,
};
inline constexpr int kSizeMetricCount = 4;

const NameChar* sizeMetricTitle(SizeMetric m);

namespace NodeFlags {
inline constexpr uint8_t IsDir = 1 << 0;
inline constexpr uint8_t IsSymlink = 1 << 1;
/// A directory that is a mount point, junction or any other reparse point. Never
/// descended into: following one both double-counts and can cycle.
inline constexpr uint8_t IsMountPoint = 1 << 2;
inline constexpr uint8_t IsHardlinkDup = 1 << 3;
/// Could not be opened — access denied, in use, or gone since it was listed.
inline constexpr uint8_t IsUnreadable = 1 << 4;
}  // namespace NodeFlags

/// Struct-of-arrays node storage with one writer and many lock-free readers.
///
/// Concurrency contract:
///  * Exactly one thread (the applier) appends nodes and mutates rolled-up totals.
///  * Readers observe `count()` with acquiring order; every node below that index
///    has been fully written, and its memory address is stable for the arena's
///    lifetime.
///  * Rolled-up totals of already-published nodes keep changing as the scan
///    proceeds. Readers may see a total that is momentarily behind, which is
///    correct for a live display. Reads are naturally atomic: these are aligned
///    64-bit loads.
///
/// Names are stored in whatever the operating system hands over — UTF-16 on
/// Windows, bytes on Linux — rather than normalised to one encoding. Both hot
/// paths stay a memcpy that way, and neither platform's awkward names are lost.
/// See name_string.h, which is where that decision is argued.
class NodeArena {
public:
    static constexpr int kNodeShift = 16;  // 65,536 nodes per chunk
    static constexpr int kNameShift = 19;  // 512K name units per chunk

    explicit NodeArena(size_t maxNodes = size_t(1) << 28,
                       size_t maxNameUnits = size_t(1) << 32);

    ChunkedArray<int32_t> parent;
    ChunkedArray<int32_t> nameOffset;
    ChunkedArray<uint16_t> nameLength;
    ChunkedArray<int16_t> kind;
    ChunkedArray<uint8_t> flags;
    ChunkedArray<int64_t> logical;
    ChunkedArray<int64_t> allocated;
    ChunkedArray<int32_t> fileCount;
    /// A directory's entries are appended in one contiguous run, so children are
    /// the index range [childStart, childStart + childCount). No child pointers,
    /// no per-node list — the tree view indexes straight into it.
    ChunkedArray<int32_t> childStart;
    ChunkedArray<int32_t> childCount;
    /// When this entry was created, in epoch seconds.
    ChunkedArray<uint32_t> addedTime;
    /// Subtree bytes whose entries were added inside each window. Rolled up the
    /// same way as `allocated`, so a folder's figure covers its whole subtree.
    ChunkedArray<int64_t> addedLastMonth;
    ChunkedArray<int64_t> addedLastSixMonths;
    ChunkedArray<int64_t> addedSinceLastScan;
    ChunkedArray<NameChar> names;

    /// Nodes safe for readers to touch.
    inline size_t count() const { return published_.load(std::memory_order_acquire); }
    /// Writer-side count, including nodes written but not yet published.
    inline size_t uncheckedCount() const { return writeCount_; }

    // ---- Writer ----

    /// Reserves `n` consecutive node slots and guarantees their backing memory.
    size_t reserveNodes(size_t n);

    /// Reserves `len` name units that do not straddle a chunk boundary, so a name
    /// is always one contiguous run.
    size_t allocateName(size_t len);

    /// Makes every node written so far visible to readers.
    inline void publish() { published_.store(writeCount_, std::memory_order_release); }

    /// Bulk-writes a name. `allocateName` guarantees the run stays inside one
    /// chunk, so this is a single memcpy rather than a per-unit chunked store.
    inline void writeName(const NameChar* src, size_t units, size_t offset) {
        if (!units) return;
        std::memcpy(names.pointer(offset), src, units * sizeof(NameChar));
    }

    // ---- Reader ----

    inline const NameChar* nameData(size_t i) const {
        return nameLength[i] ? names.pointer(size_t(uint32_t(nameOffset[i]))) : DS_T("");
    }
    inline size_t nameSize(size_t i) const { return nameLength[i]; }
    NameString name(size_t i) const;
    /// Absolute path, ready to hand back to the Win32 API.
    NameString path(size_t i) const;

    inline bool isDirectory(size_t i) const { return (flags[i] & NodeFlags::IsDir) != 0; }

    inline int64_t size(SizeMetric metric, size_t i) const {
        switch (metric) {
            case SizeMetric::AddedLastMonth: return addedLastMonth[i];
            case SizeMetric::AddedLastSixMonths: return addedLastSixMonths[i];
            case SizeMetric::AddedSinceLastScan: return addedSinceLastScan[i];
            default: return allocated[i];
        }
    }

    /// Children of `i` that are currently published. Clamped to the published
    /// count, so a reader racing the applier sees a prefix rather than garbage.
    struct Range {
        size_t begin, end;
        inline size_t size() const { return end > begin ? end - begin : 0; }
        inline bool empty() const { return end <= begin; }
    };
    inline Range children(size_t i) const {
        size_t start = size_t(uint32_t(childStart[i]));
        size_t n = size_t(uint32_t(childCount[i]));
        if (n == 0) return {0, 0};
        size_t visible = start + n;
        size_t published = count();
        if (visible > published) visible = published;
        return start < visible ? Range{start, visible} : Range{0, 0};
    }

    size_t approximateMemoryBytes() const;

private:
    std::atomic<size_t> published_{0};
    size_t writeCount_ = 0;
    size_t nameCursor_ = 0;
};

}  // namespace diskscan
