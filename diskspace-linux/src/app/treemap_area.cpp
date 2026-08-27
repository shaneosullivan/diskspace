#include "treemap_area.h"

#include <algorithm>

namespace app {

TreemapArea::TreemapArea() {
    widget_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(widget_, TRUE);
    gtk_widget_set_vexpand(widget_, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(widget_),
                                   GtkDrawingAreaDrawFunc(&TreemapArea::drawTrampoline), this,
                                   nullptr);
    g_signal_connect(widget_, "resize", G_CALLBACK(&TreemapArea::resizeTrampoline), this);

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(&TreemapArea::motionTrampoline), this);
    g_signal_connect(motion, "leave", G_CALLBACK(&TreemapArea::leaveTrampoline), this);
    gtk_widget_add_controller(widget_, motion);

    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(&TreemapArea::pressTrampoline), this);
    gtk_widget_add_controller(widget_, GTK_EVENT_CONTROLLER(click));

    // A second gesture rather than a button test inside the first: GTK routes
    // by button, and a click gesture left on its default only ever sees the
    // primary one.
    GtkGesture* secondary = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
    g_signal_connect(secondary, "pressed", G_CALLBACK(&TreemapArea::secondaryTrampoline), this);
    gtk_widget_add_controller(widget_, GTK_EVENT_CONTROLLER(secondary));
}

TreemapArea::~TreemapArea() {
    if (surface_) cairo_surface_destroy(surface_);
}

void TreemapArea::setArena(const NodeArena* arena) {
    arena_ = arena;
    invalidate();
}

void TreemapArea::setRoot(int32_t node) {
    root_ = node;
    invalidate();
}

void TreemapArea::setOptions(const TreemapOptions& options) {
    options_ = options;
    invalidate();
}

void TreemapArea::setPalette(const KindPalette* palette) {
    palette_ = palette;
    invalidate();
}

void TreemapArea::setSelected(int32_t node) {
    if (selected_ == node) return;
    selected_ = node;
    // Only the highlight changes, so the rendered pixels stand and this is a
    // repaint rather than a re-layout.
    gtk_widget_queue_draw(widget_);
}

void TreemapArea::invalidate() {
    dirty_ = true;
    if (widget_) gtk_widget_queue_draw(widget_);
}

void TreemapArea::resizeTrampoline(GtkDrawingArea*, int, int, void* self) {
    static_cast<TreemapArea*>(self)->dirty_ = true;
}

void TreemapArea::drawTrampoline(GtkDrawingArea*, cairo_t* cr, int width, int height,
                                 void* self) {
    static_cast<TreemapArea*>(self)->draw(cr, width, height);
}

void TreemapArea::renderIfNeeded(int width, int height) {
    if (!arena_ || !palette_ || width <= 0 || height <= 0) return;
    if (!dirty_ && width == renderedWidth_ && height == renderedHeight_) return;

    // Destroyed before the pixels it points at are replaced, not after. The
    // surface below borrows the vector rather than copying it, so assigning a new
    // image first frees the buffer out from under a live surface - and
    // cairo_surface_destroy writes to it on the way out, which is a crash in
    // memcpy a long way from the cause.
    if (surface_) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
    }

    image_ = Treemap::render(*arena_, root_, width, height, *palette_, options_);
    renderedWidth_ = width;
    renderedHeight_ = height;
    dirty_ = false;

    if (image_.pixels.empty()) return;

    // Wrapped, not copied: RGB24 is 0x00RRGGBB in a native-endian 32-bit word,
    // which is exactly what the renderer produced.
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, image_.width);
    surface_ = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(image_.pixels.data()), CAIRO_FORMAT_RGB24, image_.width,
        image_.height, stride);
}

void TreemapArea::draw(cairo_t* cr, int width, int height) {
    renderIfNeeded(width, height);

    // Nothing to paint when there is nothing to show: a drawing area has no
    // background of its own, so the window's own shows through, in whichever
    // theme is in force.
    if (!surface_) return;

    cairo_set_source_surface(cr, surface_, 0, 0);
    cairo_paint(cr);

    // The selected node, outlined in place rather than redrawn: drilling down
    // walks the highlight into the same picture instead of replacing it.
    if (selected_ >= 0) {
        const auto it = image_.nodeRects.find(selected_);
        if (it != image_.nodeRects.end()) {
            const RectD& r = it->second;
            cairo_set_line_width(cr, 2.0);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
            cairo_rectangle(cr, r.x + 1, r.y + 1, std::max(0.0, r.width - 2),
                            std::max(0.0, r.height - 2));
            cairo_stroke(cr);
            cairo_set_line_width(cr, 1.0);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
            cairo_rectangle(cr, r.x + 2.5, r.y + 2.5, std::max(0.0, r.width - 5),
                            std::max(0.0, r.height - 5));
            cairo_stroke(cr);
        }
    }
}

int32_t TreemapArea::nodeAt(double x, double y) const {
    if (image_.width == 0) return -1;
    const int32_t cell = image_.cellIndexAt(int(x), int(y));
    if (cell < 0 || size_t(cell) >= image_.cells.size()) return -1;
    return image_.cells[size_t(cell)].node;
}

void TreemapArea::motionTrampoline(GtkEventControllerMotion*, double x, double y, void* data) {
    auto* self = static_cast<TreemapArea*>(data);
    const int32_t node = self->nodeAt(x, y);
    if (node == self->hovered_) return;
    self->hovered_ = node;
    if (self->onHovered) self->onHovered(node);
}

void TreemapArea::leaveTrampoline(GtkEventControllerMotion*, void* data) {
    auto* self = static_cast<TreemapArea*>(data);
    if (self->hovered_ == -1) return;
    self->hovered_ = -1;
    if (self->onHovered) self->onHovered(-1);
}

void TreemapArea::secondaryTrampoline(GtkGestureClick*, int, double x, double y, void* data) {
    auto* self = static_cast<TreemapArea*>(data);
    const int32_t node = self->nodeAt(x, y);
    if (node >= 0) self->setSelected(node);
    if (self->onContextMenu) self->onContextMenu(node, x, y);
}

void TreemapArea::pressTrampoline(GtkGestureClick*, int count, double x, double y, void* data) {
    auto* self = static_cast<TreemapArea*>(data);
    const int32_t node = self->nodeAt(x, y);
    if (node < 0) return;
    if (count >= 2) {
        if (self->onActivated) self->onActivated(node);
        return;
    }
    self->setSelected(node);
    if (self->onSelected) self->onSelected(node);
}

}  // namespace app
