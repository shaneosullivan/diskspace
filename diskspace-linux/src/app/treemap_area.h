#pragma once

#include <functional>

#include "treemap.h"
#include "widgets.h"
#include "x11.h"

namespace app {

/// The treemap.
///
/// The renderer produces a buffer of 0x00RRGGBB pixels, which is byte-for-byte
/// what an X image of a 24-bit true colour visual expects on a little-endian
/// machine - the same coincidence that lets the Windows build hand the buffer
/// straight to a device independent bitmap. It goes to the screen with no
/// conversion and no copy beyond the one the server makes.
class TreemapArea {
public:
    void setBounds(const ui::Rect& bounds);
    const ui::Rect& bounds() const { return bounds_; }

    /// The arena to draw. Null clears the view.
    void setArena(const NodeArena* arena);
    void setRoot(int32_t node);
    void setOptions(const TreemapOptions& options);
    void setPalette(const KindPalette* palette);

    int32_t root() const { return root_; }
    void setSelected(int32_t node);

    /// Re-render at the next opportunity. Cheap to call repeatedly: the work
    /// happens once, when the pane next draws.
    void invalidate() { dirty_ = true; }

    void draw(ui::Canvas& canvas, const Context& context);

    bool mousePress(int x, int y, int button, int clicks);
    bool mouseMove(int x, int y);
    void mouseLeave();

    /// A node was chosen - one click on a block.
    std::function<void(int32_t)> onSelected;
    /// A node was opened - a double click, which zooms the map into it.
    std::function<void(int32_t)> onActivated;
    /// The block under the pointer changed. -1 when the pointer leaves.
    std::function<void(int32_t)> onHovered;
    /// Secondary click. The node is -1 when the click landed on nothing.
    std::function<void(int32_t, int, int)> onContextMenu;
    std::function<void()> onNeedsRedraw;

private:
    void renderIfNeeded();
    int32_t nodeAt(int x, int y) const;
    void redraw() {
        if (onNeedsRedraw) onNeedsRedraw();
    }

    ui::Rect bounds_;
    const NodeArena* arena_ = nullptr;
    const KindPalette* palette_ = nullptr;
    TreemapOptions options_;
    TreemapImage image_;

    int32_t root_ = 0;
    int32_t selected_ = -1;
    int32_t hovered_ = -1;
    bool dirty_ = true;
    int renderedWidth_ = 0;
    int renderedHeight_ = 0;
};

}  // namespace app
