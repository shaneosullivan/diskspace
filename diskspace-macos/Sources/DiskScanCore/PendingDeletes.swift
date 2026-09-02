import Foundation

/// Deletes that have not happened yet.
///
/// Deleting something takes it out of every figure at once - the treemap, the
/// list, the totals all update as though it were gone - but the file itself is
/// only moved to the trash ten seconds later. Until then it sits here and can be
/// put back exactly, which is what `TreeScanner.restore` is for.
///
/// This is a translation of core/pending_deletes.h, which the Windows and Linux
/// builds share. macOS has its own scanner rather than compiling that one, so it
/// needs its own copy of this - the two are meant to stay in step, and the
/// behaviour is deliberately described in the same words in both.
public final class PendingDeletes {
    /// How long anything sits here before it is really deleted.
    ///
    /// Long enough to notice a mistake and reach for the button, short enough
    /// that nobody is left wondering whether the delete happened.
    public static let lifetime: TimeInterval = 10

    public struct Item {
        public var node: Int32
        /// What to delete when the clock runs out.
        public var url: URL
        /// Just the leaf, which is what the pane shows.
        public var name: String
        /// Size at the moment it was removed, shown beside the name. Kept rather
        /// than read back, because by then the arena says zero.
        public var bytes: Int64
        /// Seconds left, counting down only while not paused.
        public var remaining: TimeInterval
        var removal: TreeScanner.Removal

        /// How much of the countdown is left, from 1 down to 0. What the pane
        /// draws its pie chart from.
        public var fraction: Double {
            if remaining >= PendingDeletes.lifetime { return 1 }
            if remaining <= 0 { return 0 }
            return remaining / PendingDeletes.lifetime
        }

        /// How close this is to being gone, from 0 to 1, for colouring the pie.
        ///
        /// The wedge starts blue and ends red, crossing over around the halfway
        /// mark rather than switching at it: a colour that changes gradually
        /// reads as a measurement, where one that flips reads as an alarm. Fully
        /// blue until seven seconds are left, fully red with three to go.
        public var urgency: Double {
            let t = (0.7 - fraction) / 0.4
            return min(1, max(0, t))
        }
    }

    /// The scan these entries belong to. Tokens are meaningless against any
    /// other, so starting a new scan must find this list empty.
    public weak var scanner: TreeScanner?

    /// Moves one file or folder to the trash. Supplied by the app rather than
    /// done here, so this file stays free of AppKit. Returns nil on success and
    /// something worth showing a person on failure.
    public var deleter: ((URL) -> String?)?

    /// Something was deleted for real, restored, or added: the figures have
    /// moved and the views want rebuilding.
    public var onChanged: (() -> Void)?

    /// A delete failed. The entry has been put back by then, because a file that
    /// could not be deleted is still there and the figures should say so.
    public var onFailed: ((URL, String) -> Void)?

    /// Newest first, which is where a mistake just went.
    public private(set) var items: [Item] = []
    public private(set) var isPaused = false

    public var isEmpty: Bool { items.isEmpty }
    public var count: Int { items.count }

    public init() {}

    /// Take `node` out of every figure and start its countdown.
    public func add(node: Int32, url: URL, name: String, bytes: Int64) {
        guard let scanner, node > 0 else { return }

        // Once only. Removing the same node twice reads as two entries in the
        // pane and, worse, produces two tokens - the second recording the
        // childStart and childCount that the first removal had already zeroed.
        // Restoring that one last would put the zeroes back and lose the subtree
        // from every figure.
        guard !items.contains(where: { $0.node == node }) else { return }

        // Out of the figures immediately: the point of the delay is to let
        // somebody change their mind, not to make them wait to see the effect.
        let removal = scanner.remove(node: node)
        items.insert(
            Item(node: node, url: url, name: name, bytes: bytes,
                 remaining: PendingDeletes.lifetime, removal: removal),
            at: 0)
        onChanged?()
    }

    /// Put one back, by index into `items`.
    public func undo(at index: Int) {
        guard items.indices.contains(index) else { return }
        scanner?.restore(items[index].removal)
        items.remove(at: index)
        onChanged?()
    }

    public func undoAll() {
        guard !items.isEmpty else { return }
        // Newest first, which is the reverse of the order the removals were made
        // in. The list is already newest-first, so this is simply front to back.
        //
        // It matters when one entry is an ancestor of another: the older removal
        // orphaned a subtree that the newer one then saw as empty, so putting the
        // newer one back first is the order that comes out right.
        for item in items { scanner?.restore(item.removal) }
        items.removeAll()
        onChanged?()
    }

    /// Delete everything now, without waiting for the clocks.
    public func flushAll() {
        guard !items.isEmpty else { return }
        let going = items
        items.removeAll()
        for item in going { commit(item) }
        onChanged?()
    }

    /// Forget everything without deleting or restoring, for when the scan they
    /// belong to is being thrown away.
    public func clear() { items.removeAll() }

    /// While paused the clocks stand still. Used when a question is on screen
    /// that the answer to might be "put them all back".
    public func pause() { isPaused = true }
    public func resume() { isPaused = false }

    /// Advance every clock by `elapsed` and delete anything that has run out.
    /// True when something changed and the pane should be redrawn.
    @discardableResult
    public func tick(_ elapsed: TimeInterval) -> Bool {
        guard !items.isEmpty, !isPaused, elapsed > 0 else { return false }

        for index in items.indices { items[index].remaining -= elapsed }
        let expired = items.filter { $0.remaining <= 0 }
        guard !expired.isEmpty else { return true }

        items.removeAll { $0.remaining <= 0 }
        for item in expired { commit(item) }
        onChanged?()
        return true
    }

    /// Delete `item` for real. Restores it and reports when that fails.
    private func commit(_ item: Item) {
        guard let failure = deleter?(item.url) else { return }

        // It is still there, so the figures should say so. Putting it back is the
        // only honest answer: leaving it out would show space as reclaimed that
        // never was, and there would be nothing on screen to say otherwise.
        scanner?.restore(item.removal)
        onFailed?(item.url, failure)
    }
}
