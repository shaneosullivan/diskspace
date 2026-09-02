#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace diskscan {

/// Fixed-address chunked storage.
///
/// The point of the chunking is that memory never moves. A std::vector would
/// reallocate on growth, which is fatal here: the UI reads nodes concurrently
/// with the applier appending them, and a reallocation would pull the ground out
/// from under an in-flight read. Chunks are allocated once and never resized, and
/// the chunk *table* is preallocated at full size so it never moves either.
template <typename T>
class ChunkedArray {
public:
    ChunkedArray(int shift, size_t maxChunks)
        : shift_(shift),
          chunkCapacity_(size_t(1) << shift),
          mask_(chunkCapacity_ - 1),
          maxChunks_(maxChunks) {
        table_ = static_cast<T**>(std::calloc(maxChunks, sizeof(T*)));
        if (!table_) throw std::bad_alloc();
    }

    ~ChunkedArray() {
        for (size_t i = 0; i < allocatedChunks_; ++i) std::free(table_[i]);
        std::free(table_);
    }

    ChunkedArray(const ChunkedArray&) = delete;
    ChunkedArray& operator=(const ChunkedArray&) = delete;

    /// Writer only. Must be called before any write at `index`, and therefore
    /// before the index is published to readers.
    ///
    /// The table slot is stored with release ordering so that a reader which
    /// observes a published node count is guaranteed to see the chunk pointer
    /// that node lives in.
    void ensureCapacity(size_t index) {
        size_t needed = (index >> shift_) + 1;
        if (needed > maxChunks_) throw std::bad_alloc();
        while (allocatedChunks_ < needed) {
            T* chunk = static_cast<T*>(std::calloc(chunkCapacity_, sizeof(T)));
            if (!chunk) throw std::bad_alloc();
            reinterpret_cast<std::atomic<T*>*>(&table_[allocatedChunks_])
                ->store(chunk, std::memory_order_release);
            ++allocatedChunks_;
        }
    }

    /// Direct pointer to element `i`. Valid for a contiguous run only as far as
    /// the end of its chunk, so callers must know the run does not straddle one.
    inline T* pointer(size_t i) const { return table_[i >> shift_] + (i & mask_); }

    inline T& operator[](size_t i) { return table_[i >> shift_][i & mask_]; }
    inline const T& operator[](size_t i) const { return table_[i >> shift_][i & mask_]; }

    size_t chunkCapacity() const { return chunkCapacity_; }

private:
    T** table_ = nullptr;
    int shift_;
    size_t chunkCapacity_;
    size_t mask_;
    size_t maxChunks_;
    size_t allocatedChunks_ = 0;
};

}  // namespace diskscan
