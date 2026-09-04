#include "tree_pane.h"

#include <X11/keysym.h>

#include <algorithm>
#include <iterator>

#include "file_kind.h"
#include "format.h"

namespace app {

void TreePane::setArena(const NodeArena* arena) {
    arena_ = arena;
    expanded_.clear();
    selection_.clear();
    anchor_ = -1;
    selected_ = -1;
    scrollbar_.setOffset(0);
    rebuild();
}

void TreePane::setPalette(const KindPalette* palette) { palette_ = palette; }

void TreePane::setRoot(int32_t node) {
    root_ = node;
    // The root itself is always open, or the pane would show one closed row.
    expanded_.insert(node);
    rebuild();
}

void TreePane::setMetric(SizeMetric metric) {
    metric_ = metric;
    rebuild();
}

void TreePane::setUseLogicalSize(bool logical) {
    useLogicalSize_ = logical;
    rebuild();
}

void TreePane::refresh() {
    // Which row the list should hold still, noted before the sort moves it. The
    // row height is only known while drawing, so the pixels are worked out there.
    pendingAnchor_ = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (selection_.count(rows_[i].node)) {
            pendingAnchor_ = rows_[i].node;
            pendingAnchorRow_ = int(i);
            pendingAnchorScroll_ = scrollbar_.offset();
            break;
        }
    }
    rebuild();
}

void TreePane::keepAnchorInView(int rowHeight) {
    if (pendingAnchor_ < 0 || selection_.count(pendingAnchor_) == 0) return;
    int index = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].node == pendingAnchor_) {
            index = int(i);
            break;
        }
    }
    if (index < 0) return;

    // Only a row that was on screen has a place worth keeping: a selection the
    // user has deliberately scrolled away from should stay where they left it,
    // not be dragged back on the next tick.
    const int wasAt = pendingAnchorRow_ * rowHeight;
    if (wasAt + rowHeight <= pendingAnchorScroll_ ||
        wasAt >= pendingAnchorScroll_ + bounds_.height) {
        return;
    }

    const int offset = scrollbar_.offset();
    const int y = index * rowHeight;
    // Still on screen: leave the list alone. Scrolling underneath someone who
    // can already see what they picked is its own kind of unhelpful.
    if (y >= offset && y + rowHeight <= offset + bounds_.height) return;

    scrollbar_.setOffset(y - (wasAt - pendingAnchorScroll_));
}

int64_t TreePane::sizeOf(int32_t node) const {
    if (!arena_) return 0;
    const size_t i = size_t(uint32_t(node));
    if (useLogicalSize_ && metric_ == SizeMetric::Total) return arena_->logical[i];
    return arena_->size(metric_, i);
}

void TreePane::appendRows(int32_t node, int depth) {
    if (!arena_) return;
    const size_t index = size_t(uint32_t(node));
    const auto children = arena_->children(index);

    const bool isDir = arena_->isDirectory(index);
    const bool hasChildren = isDir && !children.empty();
    const bool isExpanded = hasChildren && expanded_.count(node) != 0;

    rows_.push_back(Row{node, depth, hasChildren, isExpanded});
    if (!isExpanded) return;

    // Ranked by the measure on display, so switching to "added last month"
    // reorders the whole list rather than just recolouring it.
    std::vector<std::pair<int64_t, int32_t>> ordered;
    ordered.reserve(children.end - children.begin);
    for (size_t child = children.begin; child < children.end; ++child) {
        ordered.emplace_back(sizeOf(int32_t(child)), int32_t(child));
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;  // stable enough to stop rows jittering
    });

    for (const auto& entry : ordered) {
        const size_t child = size_t(uint32_t(entry.second));
        // Deleted, or waiting in the pending list. Either way it is gone as far
        // as this list is concerned; undoing puts the row straight back.
        if ((arena_->flags[child] & NodeFlags::IsRemoved) != 0) continue;
        // Nothing with no bytes at all, unless it is a folder that might still
        // fill up - a scan in progress would otherwise flicker rows in and out.
        if (entry.first == 0 && !arena_->isDirectory(child)) continue;
        appendRows(entry.second, depth + 1);
    }
}

void TreePane::rebuild() {
    rows_.clear();
    if (arena_ && arena_->count() > 0) appendRows(root_, 0);

    // A row that has just gone cannot stay selected, or the next Delete would
    // queue something already deleted and try to trash a path that is not there.
    if (!arena_) return;
    for (auto it = selection_.begin(); it != selection_.end();) {
        const size_t node = size_t(uint32_t(*it));
        const bool gone = node >= arena_->count() ||
                          (arena_->flags[node] & NodeFlags::IsRemoved) != 0;
        it = gone ? selection_.erase(it) : std::next(it);
    }
    if (anchor_ >= 0 && selection_.count(anchor_) == 0) anchor_ = -1;

    const size_t current = size_t(uint32_t(selected_));
    if (selected_ >= 0 && (current >= arena_->count() ||
                           (arena_->flags[current] & NodeFlags::IsRemoved) != 0)) {
        selected_ = -1;
    }
}

void TreePane::draw(ui::Canvas& canvas, const Context& context) {
    const Theme& theme = context.theme;
    canvas.fill(bounds_, theme.well);
    if (!arena_) return;

    const int rowHeight = context.metrics.rowHeight;
    const int content = int(rows_.size()) * rowHeight;
    scrollbar_.setGeometry(ui::Rect{bounds_.right() - Scrollbar::kWidth, bounds_.y,
                                    Scrollbar::kWidth, bounds_.height},
                           content, bounds_.height);

    if (pendingReveal_ >= 0) {
        scrollbar_.reveal(pendingReveal_ * rowHeight, rowHeight);
        pendingReveal_ = -1;
    }

    if (pendingAnchor_ >= 0) {
        keepAnchorInView(rowHeight);
        pendingAnchor_ = -1;
    }

    canvas.pushClip(bounds_);

    // Only the rows the viewport covers. The list can hold millions; the window
    // holds a few dozen.
    const int offset = scrollbar_.offset();
    const int firstRow = std::max(0, offset / rowHeight);
    const int lastRow = std::min(int(rows_.size()), (offset + bounds_.height) / rowHeight + 2);
    const int textRight = scrollbar_.visible() ? Scrollbar::kWidth : 0;

    for (int i = firstRow; i < lastRow; ++i) {
        const Row& row = rows_[size_t(i)];
        const size_t node = size_t(uint32_t(row.node));
        const int y = bounds_.y + i * rowHeight - offset;
        const ui::Rect rowRect{bounds_.x, y, bounds_.width, rowHeight};

        const bool isSelected = selection_.count(row.node) != 0;
        if (isSelected) {
            canvas.fill(rowRect, theme.selection);
        } else if (rowRect.contains(context.pointer.x, context.pointer.y) &&
                   bounds_.contains(context.pointer.x, context.pointer.y)) {
            canvas.fill(rowRect, theme.dark ? ui::rgba(0xff, 0xff, 0xff, 0x0e)
                                            : ui::rgba(0, 0, 0, 0x0a));
        }

        const int indent = bounds_.x + 6 + row.depth * context.metrics.indent;
        const ui::Color textColour = isSelected ? theme.selectionText : theme.text;

        // The expander, drawn as a triangle rather than themed: one shape, and it
        // lines up with the row grid at any font size.
        if (row.hasChildren) {
            const ui::Color arrow = isSelected ? theme.selectionText : theme.dimText;
            const int cx = indent + 4;
            const int cy = y + rowHeight / 2;
            if (row.expanded) {
                for (int step = 0; step < 5; ++step) {
                    canvas.fill(ui::Rect{cx - 4 + step, cy - 2 + step, 9 - 2 * step, 1}, arrow);
                }
            } else {
                for (int step = 0; step < 5; ++step) {
                    canvas.fill(ui::Rect{cx - 2 + step, cy - 4 + step, 1, 9 - 2 * step}, arrow);
                }
            }
        }

        // The kind's colour, the same one the treemap uses for it, so the two
        // panes read as one system.
        if (palette_) {
            const uint32_t colour = palette_->colour(arena_->kind[node]);
            canvas.fill(ui::Rect{indent + 12, y + rowHeight / 2 - 4, 8, 8}, 0xFF000000u | colour);
        }

        const int nameLeft = indent + 26;
        const int nameWidth =
            std::max(20, bounds_.right() - nameLeft - context.metrics.sizeColumn - 8 - textRight);
        const std::string name = displayName(arena_->nameData(node), arena_->nameSize(node));
        canvas.textIn(ui::Rect{nameLeft, y, nameWidth, rowHeight}, name, context.fonts.regular,
                      textColour);

        const std::string size = humanBytes(sizeOf(row.node));
        canvas.textIn(ui::Rect{bounds_.right() - context.metrics.sizeColumn - textRight, y,
                               context.metrics.sizeColumn - 10, rowHeight},
                      size, context.fonts.regular,
                      isSelected ? theme.selectionText : theme.dimText, ui::Align::Right);
    }

    canvas.popClip();
    scrollbar_.draw(canvas, theme, context.pointer);
}

int TreePane::rowAt(int y, const Context& context) const {
    const int index = (y - bounds_.y + scrollbar_.offset()) / context.metrics.rowHeight;
    if (index < 0 || size_t(index) >= rows_.size()) return -1;
    return index;
}

void TreePane::selectOnly(int32_t node) {
    selection_.clear();
    selection_.insert(node);
    selected_ = node;
    anchor_ = node;
}

void TreePane::selectRangeTo(int32_t node) {
    if (!arena_ || anchor_ < 0) {
        selectOnly(node);
        return;
    }

    // Only rows beside the anchor: same parent, same depth. Everything between
    // them in the list that belongs to some other folder - a subtree that
    // happens to be expanded - is passed over rather than swept up, because the
    // user cannot see what would relate those rows to the two they picked.
    const int32_t parent = arena_->parent[size_t(uint32_t(anchor_))];
    if (arena_->parent[size_t(uint32_t(node))] != parent) {
        // Not siblings, so there is no range to speak of. Treat it as a plain
        // pick rather than selecting something arbitrary.
        selectOnly(node);
        return;
    }

    int first = -1;
    int last = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].node == anchor_) first = int(i);
        if (rows_[i].node == node) last = int(i);
    }
    if (first < 0 || last < 0) {
        selectOnly(node);
        return;
    }
    if (first > last) std::swap(first, last);

    selection_.clear();
    for (int i = first; i <= last; ++i) {
        const int32_t candidate = rows_[size_t(i)].node;
        // Never the scan root. Its parent is itself, so a comparison by parent
        // alone reads it as a sibling of everything at the top level - and it is
        // the one row that can never be deleted.
        if (candidate == 0) continue;
        if (arena_->parent[size_t(uint32_t(candidate))] == parent) selection_.insert(candidate);
    }
    // The anchor stays where it was, so dragging the other end of the range
    // keeps measuring from the same place.
    selected_ = node;
}

void TreePane::toggle(int32_t node) {
    if (expanded_.count(node)) {
        expanded_.erase(node);
    } else {
        expanded_.insert(node);
    }
    rebuild();
}

bool TreePane::mousePress(int x, int y, int button, int clicks, const Context& context) {
    if (!bounds_.contains(x, y)) return false;
    if (scrollbar_.press(x, y)) {
        redraw();
        return true;
    }

    const int index = rowAt(y, context);
    if (index < 0) return true;
    const Row& row = rows_[size_t(index)];

    if (button == 3) {
        // A right click inside an existing selection acts on all of it; one
        // outside picks the row under the pointer first, which is what every
        // list does.
        if (!selection_.count(row.node)) selectOnly(row.node);
        redraw();
        if (onContextMenu) onContextMenu(row.node, x, y);
        return true;
    }
    if (button != 1) return true;

    // The expander's own column toggles rather than selects, so opening a folder
    // does not also move the selection out from under the pointer.
    const int indent = bounds_.x + 6 + row.depth * context.metrics.indent;
    if (row.hasChildren && x >= indent && x < indent + 12) {
        toggle(row.node);
        redraw();
        return true;
    }

    const int32_t node = row.node;
    if (clicks >= 2) {
        selectOnly(node);
        toggle(node);
        redraw();
        if (onActivated) onActivated(node);
        return true;
    }

    if (context.pointer.control) {
        // Adds to the selection, or takes this one out of it, and leaves
        // everything else alone.
        if (selection_.count(node)) {
            selection_.erase(node);
            if (selected_ == node) selected_ = selection_.empty() ? -1 : *selection_.begin();
        } else {
            selection_.insert(node);
            selected_ = node;
        }
        anchor_ = node;
    } else if (context.pointer.shift) {
        selectRangeTo(node);
    } else {
        selectOnly(node);
    }

    redraw();
    if (onSelected && selected_ >= 0) onSelected(selected_);
    return true;
}

bool TreePane::mouseDrag(int x, int y) {
    if (!scrollbar_.dragging()) return false;
    if (scrollbar_.drag(y)) redraw();
    return true;
}

bool TreePane::scroll(int x, int y, int steps, const Context& context) {
    if (!bounds_.contains(x, y)) return false;
    scrollbar_.scrollBy(steps * context.metrics.rowHeight * 3);
    redraw();
    return true;
}

bool TreePane::key(unsigned long keysym, const Context& context) {
    if (rows_.empty()) return false;

    int current = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].node == selected_) {
            current = int(i);
            break;
        }
    }

    switch (keysym) {
        case XK_Up:
        case XK_Down: {
            const int next = std::clamp(current < 0 ? 0 : current + (keysym == XK_Down ? 1 : -1), 0,
                                        int(rows_.size()) - 1);
            selectOnly(rows_[size_t(next)].node);
            scrollToRow(next, context);
            redraw();
            if (onSelected) onSelected(selected_);
            return true;
        }
        case XK_Right:
            if (current >= 0 && rows_[size_t(current)].hasChildren &&
                !rows_[size_t(current)].expanded) {
                toggle(selected_);
                redraw();
            }
            return true;
        case XK_Left:
            if (current >= 0 && rows_[size_t(current)].expanded) {
                toggle(selected_);
                redraw();
            } else if (current > 0 && arena_ && selected_ != root_) {
                // Already closed, so left goes to the parent, which is what every
                // tree does and what makes the key useful for getting back out.
                selectOnly(int32_t(arena_->parent[size_t(uint32_t(selected_))]));
                rebuild();
                redraw();
                if (onSelected) onSelected(selected_);
            }
            return true;
        case XK_Return:
            if (current >= 0 && onActivated) onActivated(selected_);
            return true;
        default:
            return false;
    }
}

void TreePane::selectNode(int32_t node) {
    if (selected_ == node && selection_.size() == 1) return;
    selectOnly(node);
    redraw();
}

void TreePane::scrollToRow(int index, const Context& context) {
    if (index < 0) return;
    scrollbar_.reveal(index * context.metrics.rowHeight, context.metrics.rowHeight);
}

void TreePane::revealNode(int32_t node) {
    if (!arena_ || node < 0) return;

    // Every ancestor has to be open for the row to exist at all. Walked upwards
    // and opened on the way, which is cheap: the chain is the depth of the tree,
    // not the size of it.
    std::vector<int32_t> chain;
    size_t current = size_t(uint32_t(node));
    while (current != size_t(uint32_t(root_)) && current > 0) {
        chain.push_back(int32_t(current));
        current = size_t(uint32_t(arena_->parent[current]));
    }
    for (size_t i = chain.size(); i-- > 1;) expanded_.insert(chain[i]);
    expanded_.insert(root_);
    rebuild();

    selectOnly(node);
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].node == node) {
            pendingReveal_ = int(i);
            break;
        }
    }
    redraw();
}

}  // namespace app
