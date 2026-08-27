#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace diskscan {

/// Open-addressed file-id -> node map, used to count a hard-linked file once.
///
/// This exists rather than a std::unordered_map because on the directory-walk
/// backend it holds an entry for *every* regular file, not just the multi-link
/// ones: directory enumeration hands back a file id but no link count, so
/// "have I seen this file before?" is the only available test. At three million
/// files an unordered_map costs a node allocation and a pointer chase each; this
/// costs twelve bytes of flat memory and, at a load factor of 0.5, close to one
/// cache line touched per probe.
///
/// The MFT backend does get a link count, and only inserts files that have more
/// than one — exactly what the macOS version does with `st_nlink`.
class IdMap {
public:
    IdMap() { rehash(1 << 12); }
    ~IdMap() {
        std::free(keys_);
        std::free(values_);
    }
    IdMap(const IdMap&) = delete;
    IdMap& operator=(const IdMap&) = delete;

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    /// Records the first node to reach an id. Returns false when some earlier
    /// node already claimed it — the claim is left with that one, so releasing a
    /// subtree's claims later cannot orphan an id that is still counted.
    inline bool claim(uint64_t id, int32_t node) {
        // Grown at a load factor of 0.7. Linear probing still averages under
        // three probes there, and the difference against 0.5 is 40% of a table
        // that holds an entry per file on the directory-walk backend — tens of
        // megabytes on a real disk.
        if (count_ * 10 >= capacity_ * 7) rehash(capacity_ * 2);
        size_t mask = capacity_ - 1;
        size_t i = size_t(hash(id)) & mask;
        for (;;) {
            uint64_t k = keys_[i];
            if (k == kEmpty) {
                keys_[i] = id;
                values_[i] = node;
                ++count_;
                return true;
            }
            if (k == id) return false;
            i = (i + 1) & mask;
        }
    }

    /// Keeps only the entries whose node satisfies `pred`. Rebuilt wholesale
    /// because linear probing cannot delete in place without tombstones, and a
    /// rescan is rare enough that an O(capacity) pass costs nothing that matters.
    template <typename Pred>
    void retain(Pred pred) {
        std::vector<std::pair<uint64_t, int32_t>> kept;
        kept.reserve(count_);
        for (size_t i = 0; i < capacity_; ++i) {
            if (keys_[i] != kEmpty && pred(values_[i])) kept.emplace_back(keys_[i], values_[i]);
        }
        size_t want = 1 << 12;
        while (want < kept.size() * 4) want <<= 1;
        allocate(want);
        for (auto& e : kept) claim(e.first, e.second);
    }

    void clear() { allocate(1 << 12); }

private:
    static constexpr uint64_t kEmpty = 0;

    /// splitmix64 finaliser. NTFS file reference numbers pack a sequence number
    /// into the high 16 bits and a record index into the low 48, so the low bits
    /// alone are dense and correlated; this spreads them.
    static inline uint64_t hash(uint64_t x) {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    void allocate(size_t capacity) {
        std::free(keys_);
        std::free(values_);
        keys_ = static_cast<uint64_t*>(std::calloc(capacity, sizeof(uint64_t)));
        values_ = static_cast<int32_t*>(std::calloc(capacity, sizeof(int32_t)));
        if (!keys_ || !values_) throw std::bad_alloc();
        capacity_ = capacity;
        count_ = 0;
    }

    void rehash(size_t capacity) {
        uint64_t* oldKeys = keys_;
        int32_t* oldValues = values_;
        size_t oldCapacity = capacity_;
        keys_ = nullptr;
        values_ = nullptr;
        allocate(capacity);
        for (size_t i = 0; i < oldCapacity; ++i) {
            if (oldKeys[i] != kEmpty) claim(oldKeys[i], oldValues[i]);
        }
        std::free(oldKeys);
        std::free(oldValues);
    }

    uint64_t* keys_ = nullptr;
    int32_t* values_ = nullptr;
    size_t capacity_ = 0;
    size_t count_ = 0;
};

}  // namespace diskscan
