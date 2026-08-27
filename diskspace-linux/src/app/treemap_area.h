#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>

#include "treemap.h"

namespace app {

/// The treemap, drawn into a GtkDrawingArea.
///
/// The renderer produces a buffer of 0x00RRGGBB pixels, which is byte-for-byte
/// what Cairo calls RGB24 on a little-endian machine - the same coincidence that
/// lets the Windows build hand the buffer straight to a device independent
/// bitmap. It is wrapped rather than copied.
class TreemapArea {
public:
    TreemapArea();
    ~TreemapArea();

    TreemapArea(const TreemapArea&) = delete;
    TreemapArea& operator=(const TreemapArea&) = delete;

    GtkWidget* widget() { return widget_; }

    /// The arena to draw. Null clears the view.
    void setArena(const NodeArena* arena);
    void setRoot(int32_t node);
    void setOptions(const TreemapOptions& options);
    void setPalette(const KindPalette* palette);

    int32_t root() const { return root_; }
    void setSelected(int32_t node);

    /// Re-render at the next opportunity. Cheap to call repeatedly: the work
    /// happens once, when the widget next draws.
    void invalidate();

    /// A node was chosen - one click on a block.
    std::function<void(int32_t)> onSelected;
    /// A node was opened - a double click, which zooms the map into it.
    std::function<void(int32_t)> onActivated;
    /// The block under the pointer changed. -1 when the pointer leaves.
    std::function<void(int32_t)> onHovered;
    /// Secondary click, with the position in this widget's coordinates. The node
    /// is -1 when the click landed on nothing.
    std::function<void(int32_t, double, double)> onContextMenu;

private:
    static void drawTrampoline(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                               void* self);
    static void resizeTrampoline(GtkDrawingArea* area, int width, int height, void* self);
    static void motionTrampoline(GtkEventControllerMotion* c, double x, double y, void* self);
    static void leaveTrampoline(GtkEventControllerMotion* c, void* self);
    static void pressTrampoline(GtkGestureClick* g, int count, double x, double y, void* self);
    static void secondaryTrampoline(GtkGestureClick* g, int count, double x, double y,
                                    void* self);

    void draw(cairo_t* cr, int width, int height);
    void renderIfNeeded(int width, int height);
    int32_t nodeAt(double x, double y) const;

    GtkWidget* widget_ = nullptr;
    const NodeArena* arena_ = nullptr;
    const KindPalette* palette_ = nullptr;
    TreemapOptions options_;
    TreemapImage image_;
    cairo_surface_t* surface_ = nullptr;

    int32_t root_ = 0;
    int32_t selected_ = -1;
    int32_t hovered_ = -1;
    bool dirty_ = true;
    int renderedWidth_ = 0;
    int renderedHeight_ = 0;
};

}  // namespace app
