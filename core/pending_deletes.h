#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "name_string.h"
#include "tree_scanner.h"

namespace diskscan {

/// Deletes that have not happened yet.
///
/// Deleting something takes it out of every figure at once - the treemap, the
/// list, the totals all update as though it were gone - but the file itself is
/// only moved to the trash ten seconds later. Until then it sits here and can be
/// put back exactly, which is what `TreeScanner::restore` is for.
///
/// This is the whole of the behaviour, shared by all three platforms so that
/// they cannot disagree about it. What each of them supplies is a timer to call
/// `tick`, something that actually moves a file to the trash, and the pane that
/// draws the list.
class PendingDeletes {
public:
    /// How long anything sits here before it is really deleted.
    ///
    /// Long enough to notice a mistake and reach for the button, short enough
    /// that nobody is left wondering whether the delete happened.
    static constexpr int64_t kLifetimeMillis = 10000;

    struct Item {
        int32_t node = -1;
        /// What to delete when the clock runs out.
        NameString path;
        /// Just the leaf, which is what the pane shows.
        NameString name;
        /// Size at the moment it was removed, shown beside the name. Kept rather
        /// than read back, because by then the arena says zero.
        int64_t bytes = 0;
        /// Milliseconds left, counting down only while not paused.
        int64_t remaining = kLifetimeMillis;
        /// Everything needed to put it back.
        TreeScanner::Removal removal;

        /// How much of the countdown is left, from 1 down to 0. What the pane
        /// draws its pie chart from.
        double fraction() const {
            if (remaining >= kLifetimeMillis) return 1.0;
            if (remaining <= 0) return 0.0;
            return double(remaining) / double(kLifetimeMillis);
        }
    };

    /// The scan these entries belong to. Tokens are meaningless against any
    /// other, so starting a new scan must find this list empty.
    void setScanner(TreeScanner* scanner) { scanner_ = scanner; }

    /// Take `node` out of every figure and start its countdown.
    void add(int32_t node, const NameString& path, const NameString& name, int64_t bytes);

    /// Put one back, by index into `items`.
    void undo(size_t index);
    void undoAll();

    /// Delete everything now, without waiting for the clocks.
    void flushAll();

    /// Forget everything without deleting or restoring, for when the scan they
    /// belong to is being thrown away.
    void clear() { items_.clear(); }

    /// Advance every clock by `elapsedMillis` and delete anything that has run
    /// out. True when something changed and the pane should be redrawn.
    bool tick(int64_t elapsedMillis);

    /// While paused the clocks stand still. Used when a question is on screen
    /// that the answer to might be "put them all back".
    void pause() { paused_ = true; }
    void resume() { paused_ = false; }
    bool isPaused() const { return paused_; }

    bool empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }
    /// Newest first, which is where a mistake just went.
    const std::vector<Item>& items() const { return items_; }

    /// Moves one file or folder to the trash. Supplied by the platform, because
    /// there is no portable way to do it. Returns false and fills in `error`
    /// when it could not.
    std::function<bool(const NameString& path, NameString* error)> deleter;

    /// Something was deleted for real, restored, or added: the figures have
    /// moved and the views want rebuilding.
    std::function<void()> onChanged;

    /// A delete failed. The entry has been put back by then, because a file that
    /// could not be deleted is still there and the figures should say so.
    std::function<void(const NameString& path, const NameString& error)> onFailed;

private:
    /// Delete `item` for real. Restores it and reports when that fails.
    void commit(const Item& item);

    TreeScanner* scanner_ = nullptr;
    std::vector<Item> items_;
    bool paused_ = false;
};

}  // namespace diskscan
