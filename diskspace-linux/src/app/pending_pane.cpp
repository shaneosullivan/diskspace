#include "pending_pane.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "format.h"

namespace app {
namespace {

constexpr int kPieSize = 16;
constexpr int kSizeColumn = 72;

/// A pie chart showing how much of the countdown is left.
///
/// Drawn into a small buffer and blitted rather than composed out of rectangles:
/// a wedge has a curved edge and two straight ones, and at sixteen pixels the
/// difference between an anti-aliased one and a staircase is the difference
/// between an instrument and a decoration. Nine samples per pixel, which for a
/// shape this size is a few thousand operations and happens ten times a second.
void drawPie(ui::Canvas& canvas, int x, int y, int size, double fraction, ui::Color background,
             ui::Color filled, ui::Color track) {
    if (size <= 0) return;
    fraction = std::clamp(fraction, 0.0, 1.0);

    std::vector<uint32_t> pixels(size_t(size) * size_t(size));
    const double centre = size / 2.0;
    const double radius = centre - 0.5;
    const double sweep = fraction * 2.0 * M_PI;

    auto channel = [](ui::Color colour, int shift) { return double((colour >> shift) & 0xFF); };

    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            int insideWedge = 0;
            int insideCircle = 0;
            for (int sy = 0; sy < 3; ++sy) {
                for (int sx = 0; sx < 3; ++sx) {
                    const double dx = px + (sx + 0.5) / 3.0 - centre;
                    const double dy = py + (sy + 0.5) / 3.0 - centre;
                    if (dx * dx + dy * dy > radius * radius) continue;
                    ++insideCircle;
                    // Zero at twelve o'clock, increasing clockwise, which is the
                    // direction a clock face empties in.
                    double angle = std::atan2(dx, -dy);
                    if (angle < 0) angle += 2.0 * M_PI;
                    if (angle <= sweep) ++insideWedge;
                }
            }
            const double circle = insideCircle / 9.0;
            const double wedge = insideWedge / 9.0;

            // Wedge over track over background, in one go, so the edges of both
            // are smooth against whatever is behind them.
            double r = channel(background, 16), g = channel(background, 8), b = channel(background, 0);
            const double trackCoverage = circle - wedge;
            auto mix = [](double under, double over, double coverage) {
                return under * (1.0 - coverage) + over * coverage;
            };
            r = mix(r, channel(track, 16), trackCoverage);
            g = mix(g, channel(track, 8), trackCoverage);
            b = mix(b, channel(track, 0), trackCoverage);
            r = mix(r, channel(filled, 16), wedge);
            g = mix(g, channel(filled, 8), wedge);
            b = mix(b, channel(filled, 0), wedge);

            pixels[size_t(py) * size_t(size) + size_t(px)] =
                (uint32_t(std::clamp(r, 0.0, 255.0)) << 16) |
                (uint32_t(std::clamp(g, 0.0, 255.0)) << 8) | uint32_t(std::clamp(b, 0.0, 255.0));
        }
    }
    canvas.image(x, y, pixels.data(), size, size);
}

}  // namespace

int PendingPane::rowHeight(const Context& context) const {
    return context.metrics.rowHeight + 12;
}

int PendingPane::preferredWidth(const Context& context) const {
    // Fixed: it holds a name, a size and two controls, and the only thing extra
    // width would stretch is the gap in the middle.
    return int(340 * (double(context.metrics.rowHeight) / 22.0));
}

void PendingPane::draw(ui::Canvas& canvas, const Context& context) {
    if (!model_ || model_->empty()) return;
    const Theme& theme = context.theme;
    const int padding = context.metrics.padding;
    const int header = context.metrics.rowHeight + 8;
    const int row = rowHeight(context);

    canvas.fill(bounds_, theme.surface);
    canvas.fill(ui::Rect{bounds_.x, bounds_.y, 1, bounds_.height}, theme.border);

    canvas.textIn(ui::Rect{bounds_.x + padding + 4, bounds_.y, bounds_.width - padding * 2, header},
                  "Pending Deletes", context.fonts.bold, theme.text);
    canvas.fill(ui::Rect{bounds_.x, bounds_.y + header - 1, bounds_.width, 1}, theme.border);

    // Both footer buttons are pinned to the bottom, so they are in the same place
    // whether there is one entry or forty.
    undoAllButton_.label = "Undelete All";
    deleteNowButton_.label = "Delete Now";
    const int footer = context.metrics.rowHeight + 14;
    const int buttonY = bounds_.bottom() - footer + 4;
    const int buttonHeight = context.metrics.rowHeight + 6;
    const int half = (bounds_.width - padding * 2 - 8) / 2;
    undoAllButton_.bounds = ui::Rect{bounds_.x + padding, buttonY, half, buttonHeight};
    deleteNowButton_.bounds =
        ui::Rect{bounds_.x + padding + half + 8, buttonY, half, buttonHeight};

    const int listTop = bounds_.y + header;
    const int listHeight = bounds_.height - header - footer;
    const auto& items = model_->items();
    scrollbar_.setGeometry(ui::Rect{bounds_.right() - Scrollbar::kWidth, listTop,
                                    Scrollbar::kWidth, std::max(0, listHeight)},
                           int(items.size()) * row, std::max(0, listHeight));

    canvas.pushClip(ui::Rect{bounds_.x, listTop, bounds_.width, std::max(0, listHeight)});

    undoButtons_.assign(items.size(), ui::Rect{});
    const int offset = scrollbar_.offset();
    const int first = std::max(0, offset / row);
    const int last = std::min(int(items.size()), (offset + listHeight) / row + 2);
    const int right = bounds_.right() - (scrollbar_.visible() ? Scrollbar::kWidth : 0) - padding;

    for (int i = first; i < last; ++i) {
        const diskscan::PendingDeletes::Item& item = items[size_t(i)];
        const int y = listTop + i * row - offset;
        const ui::Rect rowRect{bounds_.x, y, bounds_.width, row};
        if (rowRect.contains(context.pointer.x, context.pointer.y)) {
            canvas.fill(rowRect, theme.dark ? ui::rgba(0xff, 0xff, 0xff, 0x0e)
                                            : ui::rgba(0, 0, 0, 0x0a));
        }

        const int pieY = y + (row - kPieSize) / 2;
        const ui::Color background =
            rowRect.contains(context.pointer.x, context.pointer.y)
                ? ui::blend(theme.surface,
                            theme.dark ? ui::rgba(0xff, 0xff, 0xff, 0x0e) : ui::rgba(0, 0, 0, 0x0a))
                : theme.surface;
        // Blue while there is time, red as it runs out, crossing over around
        // half way. The curve is the model's, so the three platforms agree.
        const double urgency = item.urgency();
        auto lerp = [urgency](int calm, int urgent) {
            return uint8_t(calm + (urgent - calm) * urgency);
        };
        const ui::Color wedge =
            ui::rgb(lerp((theme.accent >> 16) & 0xFF, 0xd0), lerp((theme.accent >> 8) & 0xFF, 0x45),
                    lerp(theme.accent & 0xFF, 0x37));
        drawPie(canvas, bounds_.x + padding, pieY, kPieSize, item.fraction(), background, wedge,
                theme.border);

        // The button first, because what is left over is the name's.
        const std::string undoLabel = "Undelete";
        const int buttonWidth = canvas.textWidth(undoLabel, context.fonts.small) + 20;
        const ui::Rect button{right - buttonWidth, y + (row - context.metrics.rowHeight) / 2,
                              buttonWidth, context.metrics.rowHeight};
        undoButtons_[size_t(i)] = button;
        const bool hovered = button.contains(context.pointer.x, context.pointer.y);
        canvas.fillRounded(button, 4, hovered ? theme.surfacePressed : theme.surfaceHover);
        canvas.frame(button, theme.border);
        canvas.textIn(button, undoLabel, context.fonts.small, theme.text, ui::Align::Center);

        const int nameLeft = bounds_.x + padding + kPieSize + 8;
        const int sizeWidth = int(kSizeColumn * (double(context.metrics.rowHeight) / 22.0));
        const int nameWidth = std::max(20, button.x - 8 - sizeWidth - nameLeft);
        canvas.textIn(ui::Rect{nameLeft, y, nameWidth, row},
                      displayName(item.name.c_str(), item.name.size()), context.fonts.regular,
                      theme.text);
        canvas.textIn(ui::Rect{nameLeft + nameWidth, y, sizeWidth, row}, humanBytes(item.bytes),
                      context.fonts.small, theme.dimText, ui::Align::Right);
    }

    canvas.popClip();
    scrollbar_.draw(canvas, theme, context.pointer);

    canvas.fill(ui::Rect{bounds_.x, bounds_.bottom() - footer, bounds_.width, 1}, theme.border);
    undoAllButton_.draw(canvas, theme, context.pointer, context.fonts.regular);

    // Red, because it is the one thing in this pane that cannot be undone.
    const bool deleteHovered =
        deleteNowButton_.bounds.contains(context.pointer.x, context.pointer.y);
    canvas.fillRounded(deleteNowButton_.bounds, 5,
                       deleteHovered ? ui::rgb(0xc0, 0x39, 0x2b) : ui::rgb(0xd0, 0x45, 0x37));
    canvas.textIn(deleteNowButton_.bounds, deleteNowButton_.label, context.fonts.regular,
                  ui::rgb(0xff, 0xff, 0xff), ui::Align::Center);
}

bool PendingPane::mousePress(int x, int y, int button, const Context& context) {
    if (!model_ || model_->empty() || !bounds_.contains(x, y)) return false;
    if (button != 1) return true;

    if (scrollbar_.press(x, y)) {
        redraw();
        return true;
    }
    if (undoAllButton_.hit(x, y)) {
        model_->undoAll();
        redraw();
        return true;
    }
    if (deleteNowButton_.hit(x, y)) {
        if (onDeleteNow) onDeleteNow();
        return true;
    }
    for (size_t i = 0; i < undoButtons_.size(); ++i) {
        if (!undoButtons_[i].empty() && undoButtons_[i].contains(x, y)) {
            model_->undo(i);
            redraw();
            return true;
        }
    }
    return true;
}

bool PendingPane::mouseDrag(int x, int y) {
    if (!scrollbar_.dragging()) return false;
    if (scrollbar_.drag(y)) redraw();
    return true;
}

bool PendingPane::scroll(int x, int y, int steps, const Context& context) {
    if (!model_ || model_->empty() || !bounds_.contains(x, y)) return false;
    scrollbar_.scrollBy(steps * rowHeight(context) * 2);
    redraw();
    return true;
}

}  // namespace app
