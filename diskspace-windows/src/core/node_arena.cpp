#include "node_arena.h"

#include <vector>

namespace diskscan {

const wchar_t* sizeMetricTitle(SizeMetric m) {
    switch (m) {
        case SizeMetric::AddedLastMonth: return L"Added in Last Month";
        case SizeMetric::AddedLastSixMonths: return L"Added in Last 6 Months";
        case SizeMetric::AddedSinceLastScan: return L"Added Since Last Scan";
        default: return L"Current Size";
    }
}

NodeArena::NodeArena(size_t maxNodes, size_t maxNameUnits)
    : parent(kNodeShift, (maxNodes >> kNodeShift) + 1),
      nameOffset(kNodeShift, (maxNodes >> kNodeShift) + 1),
      nameLength(kNodeShift, (maxNodes >> kNodeShift) + 1),
      kind(kNodeShift, (maxNodes >> kNodeShift) + 1),
      flags(kNodeShift, (maxNodes >> kNodeShift) + 1),
      logical(kNodeShift, (maxNodes >> kNodeShift) + 1),
      allocated(kNodeShift, (maxNodes >> kNodeShift) + 1),
      fileCount(kNodeShift, (maxNodes >> kNodeShift) + 1),
      childStart(kNodeShift, (maxNodes >> kNodeShift) + 1),
      childCount(kNodeShift, (maxNodes >> kNodeShift) + 1),
      addedTime(kNodeShift, (maxNodes >> kNodeShift) + 1),
      addedLastMonth(kNodeShift, (maxNodes >> kNodeShift) + 1),
      addedLastSixMonths(kNodeShift, (maxNodes >> kNodeShift) + 1),
      addedSinceLastScan(kNodeShift, (maxNodes >> kNodeShift) + 1),
      names(kNameShift, (maxNameUnits >> kNameShift) + 1) {}

size_t NodeArena::reserveNodes(size_t n) {
    size_t base = writeCount_;
    if (n == 0) return base;
    size_t last = base + n - 1;
    parent.ensureCapacity(last);
    nameOffset.ensureCapacity(last);
    nameLength.ensureCapacity(last);
    kind.ensureCapacity(last);
    flags.ensureCapacity(last);
    logical.ensureCapacity(last);
    allocated.ensureCapacity(last);
    fileCount.ensureCapacity(last);
    childStart.ensureCapacity(last);
    childCount.ensureCapacity(last);
    addedTime.ensureCapacity(last);
    addedLastMonth.ensureCapacity(last);
    addedLastSixMonths.ensureCapacity(last);
    addedSinceLastScan.ensureCapacity(last);
    writeCount_ += n;
    return base;
}

size_t NodeArena::allocateName(size_t len) {
    const size_t chunkSize = size_t(1) << kNameShift;
    size_t off = nameCursor_;
    if (len == 0) return off;
    if ((off & (chunkSize - 1)) + len > chunkSize) {
        off = (off + chunkSize) & ~(chunkSize - 1);
    }
    names.ensureCapacity(off + len - 1);
    nameCursor_ = off + len;
    return off;
}

std::wstring NodeArena::name(size_t i) const {
    size_t len = nameLength[i];
    if (!len) return std::wstring();
    return std::wstring(names.pointer(size_t(uint32_t(nameOffset[i]))), len);
}

std::wstring NodeArena::path(size_t i) const {
    std::vector<size_t> chain;
    size_t cur = i;
    while (cur > 0) {
        chain.push_back(cur);
        cur = size_t(uint32_t(parent[cur]));
    }
    std::wstring out = name(0);
    for (size_t j = chain.size(); j-- > 0;) {
        if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
        out += name(chain[j]);
    }
    return out;
}

size_t NodeArena::approximateMemoryBytes() const {
    // 4+4+2+2+1+8+8+4+4+4+4+8+8+8 = 69, rounded up for alignment slack.
    return writeCount_ * 72 + nameCursor_ * sizeof(wchar_t);
}

}  // namespace diskscan
