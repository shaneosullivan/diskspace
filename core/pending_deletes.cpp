#include "pending_deletes.h"

#include <algorithm>

namespace diskscan {

void PendingDeletes::add(int32_t node, const NameString& path, const NameString& name,
                         int64_t bytes) {
    if (!scanner_ || node <= 0 || path.empty()) return;

    // Once only. Removing the same node twice reads as two entries in the pane
    // and, worse, produces two tokens - the second recording the childStart and
    // childCount that the first removal had already zeroed. Restoring that one
    // last would put the zeroes back and lose the subtree from every figure.
    // There is also no sense in it: the thing is already going.
    for (const Item& existing : items_) {
        if (existing.node == node) return;
    }

    Item item;
    item.node = node;
    item.path = path;
    item.name = name;
    item.bytes = bytes;
    item.remaining = kLifetimeMillis;
    // Out of the figures immediately: the point of the delay is to let somebody
    // change their mind, not to make them wait to see the effect.
    item.removal = scanner_->remove(node);

    // Newest first, which is where a mistake just went.
    items_.insert(items_.begin(), std::move(item));
    if (onChanged) onChanged();
}

void PendingDeletes::undo(size_t index) {
    if (index >= items_.size()) return;
    if (scanner_) scanner_->restore(items_[index].removal);
    items_.erase(items_.begin() + long(index));
    if (onChanged) onChanged();
}

void PendingDeletes::undoAll() {
    if (items_.empty()) return;
    // Newest first, which is the reverse of the order the removals were made in.
    // The list is already newest-first, so this is simply front to back.
    //
    // It matters when one entry is an ancestor of another: the older removal
    // orphaned a subtree that the newer one then saw as empty, so putting the
    // newer one back first leaves the older one restoring over the top of it,
    // which is the order that comes out right.
    if (scanner_) {
        for (const Item& item : items_) scanner_->restore(item.removal);
    }
    items_.clear();
    if (onChanged) onChanged();
}

void PendingDeletes::flushAll() {
    if (items_.empty()) return;
    // Copied, because commit can put an entry back through onFailed and the
    // list is cleared either way.
    const std::vector<Item> going = items_;
    items_.clear();
    for (const Item& item : going) commit(item);
    if (onChanged) onChanged();
}

void PendingDeletes::commit(const Item& item) {
    NameString error;
    if (deleter && deleter(item.path, &error)) return;

    // It is still there, so the figures should say so. Putting it back is the
    // only honest answer: leaving it out would show space as reclaimed that
    // never was, and there would be nothing on screen to say otherwise.
    if (scanner_) scanner_->restore(item.removal);
    if (onFailed) onFailed(item.path, error);
}

bool PendingDeletes::tick(int64_t elapsedMillis) {
    if (items_.empty() || paused_ || elapsedMillis <= 0) return false;

    std::vector<Item> expired;
    for (Item& item : items_) {
        item.remaining -= elapsedMillis;
        if (item.remaining <= 0) expired.push_back(item);
    }
    if (expired.empty()) return true;  // clocks moved, so the pane wants redrawing

    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const Item& item) { return item.remaining <= 0; }),
                 items_.end());
    for (const Item& item : expired) commit(item);
    if (onChanged) onChanged();
    return true;
}

}  // namespace diskscan
