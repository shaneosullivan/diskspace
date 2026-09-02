#pragma once

#include <functional>

#include "pending_deletes.h"
#include "widgets.h"
#include "x11.h"

namespace app {

/// The "Pending Deletes" pane, on the right, present only while something is in
/// it.
///
/// Each row is a name, its size, a pie chart counting the ten seconds down, and
/// a button to put it back. One more button is pinned to the bottom and undoes
/// all of them at once.
class PendingPane {
public:
    void setBounds(const ui::Rect& bounds) { bounds_ = bounds; }
    const ui::Rect& bounds() const { return bounds_; }
    void setModel(diskscan::PendingDeletes* model) { model_ = model; }

    /// How wide this wants to be, which is fixed: it holds a name, a size and
    /// two controls, and stretching it would only stretch the empty middle.
    int preferredWidth(const Context& context) const;

    void draw(ui::Canvas& canvas, const Context& context);

    bool mousePress(int x, int y, int button, const Context& context);
    bool mouseDrag(int x, int y);
    void mouseRelease() { scrollbar_.release(); }
    bool scroll(int x, int y, int steps, const Context& context);

    std::function<void()> onNeedsRedraw;

private:
    int rowHeight(const Context& context) const;
    void redraw() {
        if (onNeedsRedraw) onNeedsRedraw();
    }

    ui::Rect bounds_;
    Scrollbar scrollbar_;
    diskscan::PendingDeletes* model_ = nullptr;

    /// Rebuilt on every draw, like every other control here.
    std::vector<ui::Rect> undoButtons_;
    Button undoAllButton_;
};

}  // namespace app
