#pragma once

#include <windows.h>

#include <functional>
#include <vector>

#include "../core/pending_deletes.h"
#include "ui_common.h"

namespace app {

/// The "Pending Deletes" column on the right, present only while something is in
/// it.
///
/// Each row is a name, the size it had, a pie chart draining through its ten
/// seconds, and a button to put it back. One more button is pinned to the bottom
/// and undoes all of them.
///
/// The behaviour is not here: it is in core/pending_deletes.h, shared with the
/// Linux build so the two cannot disagree about what a pending delete does. This
/// draws it and reports clicks.
class PendingPane {
public:
    static PendingPane* create(HWND parent, int id);
    HWND window() const { return window_; }

    void setModel(diskscan::PendingDeletes* model) { model_ = model; }
    /// Redraw, and re-measure the scroll range. Called whenever the list changes
    /// or a clock moves.
    void refresh();

    /// Fixed: the pane holds a name, a size and two controls, and extra width
    /// would only stretch the gap between them.
    int preferredWidth() const;

    /// "Delete Now" was pressed. The window asks for confirmation and then calls
    /// flushAll: the pane does not delete anything itself, and does not put a
    /// dialog on screen either.
    std::function<void()> onDeleteNow;

private:
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT handle(UINT message, WPARAM w, LPARAM l);
    void paint(HDC dc);
    void layout();
    /// Index of the Undelete button under the point, or -1.
    int hitTestUndo(int x, int y) const;
    int rowHeight() const;

    HWND window_ = nullptr;
    ui::BackBuffer buffer_;
    diskscan::PendingDeletes* model_ = nullptr;

    std::vector<RECT> undoButtons_;
    RECT undoAllButton_{};
    RECT deleteNowButton_{};
    int scrollY_ = 0;
    int contentHeight_ = 0;
    int hoveredUndo_ = -1;
    bool hoveringUndoAll_ = false;
    bool hoveringDeleteNow_ = false;
};

}  // namespace app
