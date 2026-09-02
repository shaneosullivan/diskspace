// Does undoing a delete put every figure back exactly?
//
// This exists because the pending-delete feature undoes removals, and a removal
// that is very slightly not reversible corrupts the totals in a way nothing
// reports: the treemap keeps drawing, the numbers keep looking plausible, and
// they are wrong. The arithmetic is written once and applied with a sign, which
// makes drift unlikely - this makes it checkable.
//
// Built as part of the Linux tree because that is where a test harness already
// lives, but everything under test is in core/ and shared with Windows.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "file_kind.h"
#include "tree_scanner.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "\x1b[32mPASS\x1b[0m" : "\x1b[31mFAIL\x1b[0m", what);
    if (!ok) ++failures;
}

/// Every figure the application can show, flattened so two states can be
/// compared in one go rather than field by field.
struct Snapshot {
    std::vector<int64_t> values;

    bool operator==(const Snapshot& other) const { return values == other.values; }
};

Snapshot capture(const diskscan::TreeScanner& scanner) {
    const diskscan::NodeArena& arena = scanner.arena();
    Snapshot out;
    // Every node, not just the root: a removal that got the root right and an
    // intermediate directory wrong would still be a bug, and one the treemap
    // would draw.
    for (size_t i = 0; i < arena.count(); ++i) {
        out.values.push_back(arena.logical[i]);
        out.values.push_back(arena.allocated[i]);
        out.values.push_back(arena.fileCount[i]);
        out.values.push_back(arena.addedLastMonth[i]);
        out.values.push_back(arena.addedLastSixMonths[i]);
        out.values.push_back(arena.addedSinceLastScan[i]);
        out.values.push_back(arena.childStart[i]);
        out.values.push_back(arena.childCount[i]);
    }
    // And the per-kind table, which is maintained separately and is the easiest
    // thing to leave behind.
    for (const auto& total : scanner.kindTotals(diskscan::SizeMetric::Total, false)) {
        out.values.push_back(total.kind);
        out.values.push_back(total.bytes);
        out.values.push_back(total.count);
    }
    return out;
}

/// Node indices worth removing: the root's children, a deep directory, and a
/// file, so the test covers a subtree, a leaf and something in between.
std::vector<int32_t> interestingNodes(const diskscan::NodeArena& arena) {
    std::vector<int32_t> out;
    int32_t deepestDirectory = -1;
    int32_t aFile = -1;
    for (size_t i = 1; i < arena.count(); ++i) {
        const bool isDirectory = (arena.flags[i] & diskscan::NodeFlags::IsDir) != 0;
        if (isDirectory && arena.childCount[i] > 0) deepestDirectory = int32_t(i);
        if (!isDirectory && aFile < 0 && arena.allocated[i] > 0) aFile = int32_t(i);
        if (arena.parent[i] == 0) out.push_back(int32_t(i));
    }
    if (deepestDirectory > 0) out.push_back(deepestDirectory);
    if (aFile > 0) out.push_back(aFile);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: reversibility <directory>\n");
        return 2;
    }
    diskscan::FileKind::warmUp();

    const diskscan::NameString root = argv[1];
    diskscan::ScanOptions options;
    diskscan::TreeScanner scanner(options);
    const diskscan::ScanResult result = scanner.scan(root);
    if (scanner.arena().count() <= 1) {
        std::fprintf(stderr, "nothing scanned at %s\n", argv[1]);
        return 2;
    }
    std::printf("scanned %s: %zu entries, %lld bytes\n\n", argv[1], scanner.arena().count(),
                (long long)scanner.arena().allocated[0]);
    (void)result;

    const Snapshot original = capture(scanner);
    const std::vector<int32_t> nodes = interestingNodes(scanner.arena());

    std::printf("remove then restore, one node at a time\n");
    bool allExact = true;
    for (int32_t node : nodes) {
        const diskscan::TreeScanner::Removal token = scanner.remove(node);
        const Snapshot removed = capture(scanner);
        if (removed == original) {
            // Removing something with bytes in it has to change something, or the
            // test is measuring nothing.
            if (scanner.arena().allocated[size_t(uint32_t(node))] != 0) allExact = false;
        }
        scanner.restore(token);
        if (!(capture(scanner) == original)) allExact = false;
    }
    check(allExact, "every figure returns to what it was");

    std::printf("\nremoving several, then restoring in reverse\n");
    std::vector<diskscan::TreeScanner::Removal> tokens;
    for (int32_t node : nodes) tokens.push_back(scanner.remove(node));
    const Snapshot afterAll = capture(scanner);
    check(!(afterAll == original), "removing several changes the totals");
    for (size_t i = tokens.size(); i-- > 0;) scanner.restore(tokens[i]);
    check(capture(scanner) == original, "restoring them all puts everything back");

    std::printf("\nthe same removal twice running\n");
    const diskscan::TreeScanner::Removal first = scanner.remove(nodes.front());
    const Snapshot afterFirst = capture(scanner);
    scanner.restore(first);
    const diskscan::TreeScanner::Removal second = scanner.remove(nodes.front());
    check(capture(scanner) == afterFirst, "leaves the same state as the first");
    scanner.restore(second);
    check(capture(scanner) == original, "and undoes to the same place");

    std::printf("\nrestoring something never removed\n");
    diskscan::TreeScanner::Removal empty;
    scanner.restore(empty);
    check(capture(scanner) == original, "an invalid token does nothing");

    std::printf("\n%s\n", failures == 0 ? "all reversibility checks passed"
                                        : "\x1b[31mreversibility checks failed\x1b[0m");
    return failures == 0 ? 0 : 1;
}
