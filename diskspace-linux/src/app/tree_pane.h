#pragma once

#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

#include "node_arena.h"
#include "treemap.h"
#include "widgets.h"
#include "x11.h"

namespace app {

using diskscan::NodeArena;
namespace NodeFlags = diskscan::NodeFlags;
using diskscan::SizeMetric;

/// The file list: every folder ranked by whichever measure is selected, with the
/// biggest first and folders expandable in place.
///
/// Drawn rather than assembled out of widgets, which was true under GTK too and
/// is the reason losing the toolkit cost this file so little. A scan reaches
/// millions of nodes, and a list model that materialised a widget per row - or
/// even per visible row, rebuilt ten times a second while the scan runs - would
/// spend more time in the toolkit than in the scan. Only the rows on screen are
/// ever touched.
class TreePane {
public:
    void setBounds(const ui::Rect& bounds) { bounds_ = bounds; }
    const ui::Rect& bounds() const { return bounds_; }

    void setArena(const NodeArena* arena);
    void setPalette(const KindPalette* palette);
    void setRoot(int32_t node);
    void setMetric(SizeMetric metric);
    void setUseLogicalSize(bool logical);

    /// Rebuild the visible rows from the arena. Called on every scan tick.
    void refresh();

    void selectNode(int32_t node);
    /// The one row that actions without a selection apply to: the last one
    /// touched. -1 when nothing is selected.
    int32_t selected() const { return selected_; }
    /// Every selected row, which is usually just the one. Actions that can work
    /// on several - deleting, most obviously - use this.
    const std::unordered_set<int32_t>& selection() const { return selection_; }

    /// Expand from the root down to `node` so it can be shown, then select it.
    void revealNode(int32_t node);

    void draw(ui::Canvas& canvas, const Context& context);

    /// Each returns true when the pane handled the event.
    bool mousePress(int x, int y, int button, int clicks, const Context& context);
    bool mouseDrag(int x, int y);
    void mouseRelease() { scrollbar_.release(); }
    bool scroll(int x, int y, int steps, const Context& context);
    /// Up, down, left and right, when the list has the keyboard.
    bool key(unsigned long keysym, const Context& context);

    std::function<void(int32_t)> onSelected;
    std::function<void(int32_t)> onActivated;
    std::function<void(int32_t, int, int)> onContextMenu;
    /// Something changed that needs a repaint.
    std::function<void()> onNeedsRedraw;

private:
    struct Row {
        int32_t node = 0;
        int depth = 0;
        bool hasChildren = false;
        bool expanded = false;
    };

    void appendRows(int32_t node, int depth);
    void rebuild();
    int64_t sizeOf(int32_t node) const;
    /// Index of the row at window coordinate `y`, or -1.
    int rowAt(int y, const Context& context) const;
    void toggle(int32_t node);
    /// Select from the anchor to `node`, taking only rows that sit beside the
    /// anchor: same parent, same depth. A range that swept up whatever happened
    /// to be expanded in between would select things the user cannot see the
    /// relationship to.
    void selectRangeTo(int32_t node);
    void selectOnly(int32_t node);
    void scrollToRow(int index, const Context& context);
    void redraw() {
        if (onNeedsRedraw) onNeedsRedraw();
    }

    ui::Rect bounds_;
    Scrollbar scrollbar_;

    const NodeArena* arena_ = nullptr;
    const KindPalette* palette_ = nullptr;

    std::vector<Row> rows_;
    std::unordered_set<int32_t> expanded_;
    /// Where a shift-click measures from: the last row picked without shift.
    ///
    /// Kept separately from the selection because a range has to be re-drawn from
    /// its starting point every time the other end moves, and the selection at
    /// that moment is the result rather than the input.
    int32_t anchor_ = -1;
    std::unordered_set<int32_t> selection_;
    int32_t root_ = 0;
    int32_t selected_ = -1;
    /// A row to bring into view at the next draw.
    ///
    /// Deferred because revealing needs the row height, which is scaled and
    /// lives in the draw context - and the caller that wants a node revealed
    /// (the treemap, usually) has no business holding one.
    int pendingReveal_ = -1;
    SizeMetric metric_ = SizeMetric::Total;
    bool useLogicalSize_ = false;
};

}  // namespace app
