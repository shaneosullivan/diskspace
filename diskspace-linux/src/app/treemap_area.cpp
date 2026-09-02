#include "treemap_area.h"

#include <algorithm>

namespace app {

void TreemapArea::setBounds(const ui::Rect& bounds) {
    if (bounds.width != bounds_.width || bounds.height != bounds_.height) dirty_ = true;
    bounds_ = bounds;
}

void TreemapArea::setArena(const NodeArena* arena) {
    arena_ = arena;
    root_ = 0;
    selected_ = -1;
    hovered_ = -1;
    dirty_ = true;
}

void TreemapArea::setRoot(int32_t node) {
    root_ = node;
    dirty_ = true;
}

void TreemapArea::setOptions(const TreemapOptions& options) {
    options_ = options;
    dirty_ = true;
}

void TreemapArea::setPalette(const KindPalette* palette) {
    palette_ = palette;
    dirty_ = true;
}

void TreemapArea::setSelected(int32_t node) {
    if (selected_ == node) return;
    selected_ = node;
    // Only the highlight changes, so the rendered pixels stand and this is a
    // repaint rather than a re-render.
    redraw();
}

void TreemapArea::renderIfNeeded() {
    const int width = bounds_.width;
    const int height = bounds_.height;
    if (width <= 0 || height <= 0 || !arena_ || !palette_) return;
    if (!dirty_ && renderedWidth_ == width && renderedHeight_ == height) return;

    image_ = Treemap::render(*arena_, root_, width, height, *palette_, options_);
    renderedWidth_ = width;
    renderedHeight_ = height;
    dirty_ = false;
}

void TreemapArea::draw(ui::Canvas& canvas, const Context& context) {
    renderIfNeeded();

    if (image_.width == 0 || image_.pixels.empty()) {
        canvas.fill(bounds_, context.theme.well);
        return;
    }

    canvas.pushClip(bounds_);
    canvas.image(bounds_.x, bounds_.y, image_.pixels.data(), image_.width, image_.height);

    // The selected node, outlined in place rather than redrawn: drilling down
    // walks the highlight into the same picture instead of replacing it.
    //
    // Two frames, light over dark, so the outline stays visible whatever colour
    // the block underneath happens to be.
    if (selected_ >= 0) {
        const auto found = image_.nodeRects.find(selected_);
        if (found != image_.nodeRects.end()) {
            const RectD& r = found->second;
            const ui::Rect outer{bounds_.x + int(r.x) + 1, bounds_.y + int(r.y) + 1,
                                 std::max(0, int(r.width) - 2), std::max(0, int(r.height) - 2)};
            canvas.frame(outer, ui::rgba(0xff, 0xff, 0xff, 0xf0));
            canvas.frame(outer.inset(1), ui::rgba(0, 0, 0, 0xb0));
        }
    }

    canvas.popClip();
}

int32_t TreemapArea::nodeAt(int x, int y) const {
    if (image_.width == 0) return -1;
    const int32_t cell = image_.cellIndexAt(x - bounds_.x, y - bounds_.y);
    if (cell < 0 || size_t(cell) >= image_.cells.size()) return -1;
    return image_.cells[size_t(cell)].node;
}

bool TreemapArea::mouseMove(int x, int y) {
    if (!bounds_.contains(x, y)) {
        mouseLeave();
        return false;
    }
    const int32_t node = nodeAt(x, y);
    if (node == hovered_) return true;
    hovered_ = node;
    if (onHovered) onHovered(node);
    return true;
}

void TreemapArea::mouseLeave() {
    if (hovered_ == -1) return;
    hovered_ = -1;
    if (onHovered) onHovered(-1);
}

bool TreemapArea::mousePress(int x, int y, int button, int clicks) {
    if (!bounds_.contains(x, y)) return false;
    const int32_t node = nodeAt(x, y);

    if (button == 3) {
        if (node >= 0) setSelected(node);
        if (onContextMenu) onContextMenu(node, x, y);
        return true;
    }
    if (button != 1 || node < 0) return true;

    if (clicks >= 2) {
        if (onActivated) onActivated(node);
        return true;
    }
    setSelected(node);
    if (onSelected) onSelected(node);
    return true;
}

}  // namespace app
