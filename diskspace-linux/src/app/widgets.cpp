#include "widgets.h"

#include <algorithm>

namespace app {

Fonts Fonts::load(ui::Display* display) {
    Fonts fonts;
    // Ten and a half points is what a GNOME desktop uses by default, and the
    // scale the display resolved is already folded into it.
    fonts.regular = display->font(10.5);
    fonts.bold = display->font(10.5, ui::Weight::Bold);
    fonts.small = display->font(9.0);
    fonts.title = display->font(15.0, ui::Weight::Bold);
    return fonts;
}

Metrics Metrics::forScale(double scale) {
    Metrics metrics;
    auto scaled = [scale](int value) { return int(value * scale + 0.5); };
    metrics.rowHeight = scaled(22);
    metrics.headerHeight = scaled(44);
    metrics.statusHeight = scaled(26);
    metrics.padding = scaled(8);
    metrics.indent = scaled(16);
    metrics.sizeColumn = scaled(110);
    return metrics;
}

// =============================================================================
// Button
// =============================================================================

int Button::preferredWidth(ui::Canvas& canvas, const ui::Font& font) const {
    return canvas.textWidth(label, font) + 24;
}

void Button::draw(ui::Canvas& canvas, const Theme& theme, const Pointer& pointer,
                  const ui::Font& font) const {
    if (bounds.empty()) return;
    const bool hovered = enabled && bounds.contains(pointer.x, pointer.y);
    const bool pressed = hovered && pointer.down;

    ui::Color fill = theme.surface;
    if (toggled) fill = theme.surfacePressed;
    if (pressed) {
        fill = theme.surfacePressed;
    } else if (hovered) {
        fill = theme.surfaceHover;
    }

    if (!flat || hovered || toggled) canvas.fillRounded(bounds, 5, fill);
    if (!flat) {
        // The border after the fill, as one-pixel lines rather than a rounded
        // outline: at a five pixel radius the difference is invisible and this
        // needs no second pass over the corners.
        canvas.frame(bounds, theme.border);
    }

    const ui::Color colour = enabled ? theme.text : theme.dimText;
    canvas.textIn(bounds, label, font, colour, ui::Align::Center);
}

// =============================================================================
// Scrollbar
// =============================================================================

void Scrollbar::setGeometry(const ui::Rect& track, int contentHeight, int viewportHeight) {
    track_ = track;
    contentHeight_ = contentHeight;
    viewportHeight_ = viewportHeight;
    setOffset(offset_);
}

int Scrollbar::maxOffset() const { return std::max(0, contentHeight_ - viewportHeight_); }

void Scrollbar::setOffset(int value) { offset_ = std::clamp(value, 0, maxOffset()); }

void Scrollbar::reveal(int top, int height) {
    if (top < offset_) {
        setOffset(top);
    } else if (top + height > offset_ + viewportHeight_) {
        setOffset(top + height - viewportHeight_);
    }
}

ui::Rect Scrollbar::thumb() const {
    if (!visible()) return ui::Rect{};
    const double fraction = double(viewportHeight_) / double(contentHeight_);
    const int height = std::max(24, int(track_.height * fraction));
    const int travel = track_.height - height;
    const int max = maxOffset();
    const int y = max > 0 ? track_.y + int(double(travel) * double(offset_) / double(max))
                          : track_.y;
    return ui::Rect{track_.x, y, track_.width, height};
}

void Scrollbar::draw(ui::Canvas& canvas, const Theme& theme, const Pointer& pointer) const {
    if (!visible()) return;
    const ui::Rect bar = thumb();
    const bool hovered = track_.contains(pointer.x, pointer.y) || dragging_;
    // Narrow until the pointer is near it, which is what every desktop does now
    // and which keeps a scrollbar from competing with the list beside it.
    const int inset = hovered ? 0 : track_.width / 3;
    const ui::Rect drawn{bar.x + inset, bar.y, bar.width - inset, bar.height};
    canvas.fillRounded(drawn, drawn.width / 2,
                       hovered ? (theme.dark ? ui::rgba(0xff, 0xff, 0xff, 0x88)
                                             : ui::rgba(0, 0, 0, 0x66))
                               : (theme.dark ? ui::rgba(0xff, 0xff, 0xff, 0x44)
                                             : ui::rgba(0, 0, 0, 0x33)));
}

bool Scrollbar::press(int x, int y) {
    if (!visible() || !track_.contains(x, y)) return false;
    const ui::Rect bar = thumb();
    if (bar.contains(x, y)) {
        dragging_ = true;
        grabOffset_ = y - bar.y;
        return true;
    }
    // A click on the track jumps the thumb to the pointer and then drags, which
    // is what a click on empty track should do when there are no arrows.
    dragging_ = true;
    grabOffset_ = bar.height / 2;
    drag(y);
    return true;
}

bool Scrollbar::drag(int y) {
    if (!dragging_ || !visible()) return false;
    const ui::Rect bar = thumb();
    const int travel = track_.height - bar.height;
    if (travel <= 0) return false;
    const int position = std::clamp(y - grabOffset_ - track_.y, 0, travel);
    const int before = offset_;
    setOffset(int(double(position) / double(travel) * double(maxOffset())));
    return offset_ != before;
}

// =============================================================================
// Menu
// =============================================================================

void Menu::open(const std::vector<MenuItem>& items, int x, int y) {
    items_ = items;
    wantX_ = x;
    wantY_ = y;
    open_ = true;
    measured_ = false;
}

void Menu::layout(ui::Canvas& canvas, const ui::Font& font) {
    if (!open_ || measured_) return;
    int width = 140;
    int height = kPadding * 2;
    for (const MenuItem& item : items_) {
        if (item.label.empty()) {
            height += kSeparatorHeight;
            continue;
        }
        height += kItemHeight;
        width = std::max(width, canvas.textWidth(item.label, font) + 52);
    }
    // Clamped inside the window, which is what an overlay menu has instead of
    // the ability to hang over the edge.
    const int x = std::clamp(wantX_, 0, std::max(0, canvas.width() - width));
    const int y = std::clamp(wantY_, 0, std::max(0, canvas.height() - height));
    bounds_ = ui::Rect{x, y, width, height};
    measured_ = true;
}

void Menu::draw(ui::Canvas& canvas, const Theme& theme, const Pointer& pointer,
                const ui::Font& font) const {
    if (!open_ || bounds_.empty()) return;

    // A shadow, as three progressively fainter rounded rectangles. Cheap, and
    // enough to lift the menu off whatever is behind it.
    for (int i = 3; i >= 1; --i) {
        canvas.fillRounded(ui::Rect{bounds_.x - i, bounds_.y - i + 2, bounds_.width + 2 * i,
                                    bounds_.height + 2 * i},
                           7 + i, ui::rgba(0, 0, 0, (unsigned char)(10 + 6 * (4 - i))));
    }
    canvas.fillRounded(bounds_, 7, theme.surface);
    canvas.frame(bounds_, theme.border);

    int y = bounds_.y + kPadding;
    for (const MenuItem& item : items_) {
        if (item.label.empty()) {
            canvas.fill(ui::Rect{bounds_.x + 8, y + kSeparatorHeight / 2, bounds_.width - 16, 1},
                        theme.border);
            y += kSeparatorHeight;
            continue;
        }
        const ui::Rect row{bounds_.x + 4, y, bounds_.width - 8, kItemHeight};
        const bool hovered = item.enabled && row.contains(pointer.x, pointer.y);
        if (hovered) canvas.fillRounded(row, 4, theme.selection);

        const ui::Color colour = !item.enabled ? theme.dimText
                                 : hovered     ? theme.selectionText
                                               : theme.text;
        if (item.checkable && item.checked) {
            // A tick, as two strokes. A glyph would need a font that has one.
            const int cx = row.x + 12;
            const int cy = row.y + kItemHeight / 2;
            for (int i = 0; i < 3; ++i) canvas.fill(ui::Rect{cx - 4 + i, cy + i - 1, 2, 2}, colour);
            for (int i = 0; i < 5; ++i) canvas.fill(ui::Rect{cx - 1 + i, cy + 2 - i, 2, 2}, colour);
        }
        canvas.textIn(ui::Rect{row.x + 26, row.y, row.width - 34, row.height}, item.label, font,
                      colour);
        y += kItemHeight;
    }
}

bool Menu::click(int x, int y) {
    if (!open_) return false;
    if (!bounds_.contains(x, y)) {
        open_ = false;
        return true;  // consumed: a click that dismisses a menu does nothing else
    }
    int top = bounds_.y + kPadding;
    for (const MenuItem& item : items_) {
        if (item.label.empty()) {
            top += kSeparatorHeight;
            continue;
        }
        if (y >= top && y < top + kItemHeight) {
            open_ = false;
            if (item.enabled && item.action) item.action();
            return true;
        }
        top += kItemHeight;
    }
    open_ = false;
    return true;
}

}  // namespace app
