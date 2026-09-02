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
#include "pending_deletes.h"
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
        // Including the flags, because one of them is what hides the row.
        out.values.push_back(arena.flags[i]);
    }
    // And the per-kind table, which is maintained separately and is the easiest
    // thing to leave behind.
    // And how many entries are left, which is what the status line counts.
    out.values.push_back(scanner.liveNodeCount());
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

    // ---- pending deletes ---------------------------------------------------
    //
    // The same guarantees, through the object the interface actually drives, and
    // with a deleter that only records what it was asked to remove: a test that
    // trashes files to prove it can is one nobody will run twice.
    std::printf("\npending deletes\n");
    diskscan::PendingDeletes pending;
    pending.setScanner(&scanner);

    std::vector<diskscan::NameString> deleted;
    bool deleterShouldFail = false;
    pending.deleter = [&](const diskscan::NameString& path, diskscan::NameString* error) {
        if (deleterShouldFail) {
            if (error) *error = "Permission denied";
            return false;
        }
        deleted.push_back(path);
        return true;
    };
    int failuresReported = 0;
    pending.onFailed = [&](const diskscan::NameString&, const diskscan::NameString&) {
        ++failuresReported;
    };

    const auto queue = [&](int32_t node) {
        const size_t i = size_t(uint32_t(node));
        pending.add(node, scanner.arena().path(i),
                    diskscan::NameString(scanner.arena().nameData(i), scanner.arena().nameSize(i)),
                    scanner.arena().allocated[i]);
    };

    const auto isHidden = [&](int32_t node) {
        return (scanner.arena().flags[size_t(uint32_t(node))] &
                diskscan::NodeFlags::IsRemoved) != 0;
    };

    queue(nodes.front());
    check(pending.size() == 1, "adding one queues it");
    check(!(capture(scanner) == original), "and takes it out of the figures at once");
    check(isHidden(nodes.front()), "and marks it gone, so no row is left behind");
    pending.undo(0);
    check(pending.empty() && capture(scanner) == original, "undoing puts it back exactly");
    check(!isHidden(nodes.front()), "and the row comes back");

    // The count of what is left has to fall by the whole subtree, not by one.
    // Deriving folders as "every slot ever allocated, less the files" made the
    // folder count go up when a file was deleted.
    {
        const int64_t before = scanner.liveNodeCount();
        const int32_t node = nodes.front();
        // isDescendant counts a node as its own descendant, so this includes the
        // entry itself without adding it separately.
        int64_t subtree = 0;
        for (size_t i = 1; i < scanner.arena().count(); ++i) {
            if (scanner.isDescendant(int32_t(i), node)) ++subtree;
        }
        queue(node);
        check(scanner.liveNodeCount() == before - subtree,
              "the entry count falls by the whole subtree");
        pending.undoAll();
        check(scanner.liveNodeCount() == before, "and comes back whole");
    }

    queue(nodes.front());
    queue(nodes.front());
    check(pending.size() == 1, "the same entry cannot be queued twice");
    pending.undoAll();

    // A folder and something inside it, which is what selecting several rows can
    // now produce. Removing the folder already takes the file out of every
    // figure; queuing the file as well would subtract the same bytes twice.
    {
        int32_t parent = -1;
        int32_t child = -1;
        for (size_t i = 1; i < scanner.arena().count() && parent < 0; ++i) {
            if ((scanner.arena().flags[i] & diskscan::NodeFlags::IsDir) == 0) continue;
            if (scanner.arena().childCount[i] <= 0) continue;
            parent = int32_t(i);
            child = scanner.arena().childStart[i];
        }
        if (parent > 0 && child > 0) {
            queue(parent);
            queue(child);
            check(pending.size() == 1, "a folder and its child queue only once");
            pending.undoAll();
            check(capture(scanner) == original, "and undo to exactly where they started");

            // The other order too: the child first, then the folder around it.
            queue(child);
            queue(parent);
            check(pending.size() == 1, "and only once the other way round");
            pending.undoAll();
            check(capture(scanner) == original, "still undoing exactly");
        }
    }

    // Only siblings here. The list refuses anything that contains, or is
    // contained by, something already queued, so a set that included a deep
    // directory and a file inside one of these would legitimately come out
    // shorter - which is the subject of the checks above, not this one.
    std::vector<int32_t> siblings;
    for (size_t i = 1; i < scanner.arena().count(); ++i) {
        if (scanner.arena().parent[i] == 0) siblings.push_back(int32_t(i));
    }
    for (int32_t node : siblings) queue(node);
    check(pending.size() == siblings.size(), "several siblings can be queued");
    check(pending.items().front().node == siblings.back(), "newest is first in the list");
    pending.undoAll();
    check(pending.empty() && capture(scanner) == original, "undoing all puts everything back");

    // From here on the deletes go through, so each block needs a node of its own:
    // queuing one that has already been deleted removes nothing and proves
    // nothing. There are as many top-level entries as blocks that need one.
    size_t nextVictim = 0;
    const auto victim = [&]() -> int32_t {
        return nextVictim < siblings.size() ? siblings[nextVictim++] : -1;
    };
    check(siblings.size() >= 3, "the tree has enough top-level entries to delete from");

    const int32_t expiring = victim();
    queue(expiring);
    pending.pause();
    pending.tick(60000);
    check(pending.size() == 1 && deleted.empty(), "a paused countdown does not run out");
    pending.resume();
    const Snapshot whileQueued = capture(scanner);
    pending.tick(diskscan::PendingDeletes::kLifetimeMillis);
    check(pending.empty() && deleted.size() == 1, "running out deletes it for real");
    check(capture(scanner) == whileQueued, "and leaves the figures where they already were");
    // The mark belongs to the removal, not to the pending entry, so the row does
    // not come back at zero bytes once the entry is gone from the list.
    check(isHidden(expiring), "and it stays out of the list afterwards");

    deleted.clear();
    queue(victim());
    pending.flushAll();
    check(pending.empty() && deleted.size() == 1, "flushing deletes without waiting");

    // A delete that fails has to put the entry back, or the figures would show
    // space reclaimed that never was and nothing on screen would say otherwise.
    deleted.clear();
    deleterShouldFail = true;
    const Snapshot beforeFailure = capture(scanner);
    const int32_t doomed = victim();
    queue(doomed);
    pending.flushAll();
    check(deleted.empty() && failuresReported == 1, "a failed delete is reported");
    check(capture(scanner) == beforeFailure, "and the entry comes back into the figures");
    check(!isHidden(doomed), "and its row with them");
    deleterShouldFail = false;

    std::printf("\n%s\n", failures == 0 ? "all reversibility checks passed"
                                        : "\x1b[31mreversibility checks failed\x1b[0m");
    return failures == 0 ? 0 : 1;
}
