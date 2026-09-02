#include "pending_pane.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace app {
namespace {

constexpr const wchar_t* kClassName = L"DiskSpacePendingPane";
constexpr int kHeaderHeight = 30;
constexpr int kFooterHeight = 42;
constexpr int kRowHeight = 34;
constexpr int kPieSize = 16;

/// A pie chart showing how much of the countdown is left.
///
/// Composed in memory and blitted rather than drawn with GDI's Pie, which is not
/// anti-aliased: at sixteen pixels a staircase edge is the difference between an
/// instrument and a decoration. Nine samples a pixel, a few thousand operations,
/// ten times a second.
void drawPie(HDC dc, int x, int y, int size, double fraction, COLORREF background,
             COLORREF filled, COLORREF track) {
    if (size <= 0) return;
    fraction = (std::max)(0.0, (std::min)(1.0, fraction));

    std::vector<uint32_t> pixels(size_t(size) * size_t(size));
    const double centre = size / 2.0;
    const double radius = centre - 0.5;
    const double sweep = fraction * 2.0 * 3.14159265358979323846;

    auto red = [](COLORREF c) { return double(GetRValue(c)); };
    auto green = [](COLORREF c) { return double(GetGValue(c)); };
    auto blue = [](COLORREF c) { return double(GetBValue(c)); };

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
                    if (angle < 0) angle += 2.0 * 3.14159265358979323846;
                    if (angle <= sweep) ++insideWedge;
                }
            }
            const double circle = insideCircle / 9.0;
            const double wedge = insideWedge / 9.0;
            const double trackCoverage = circle - wedge;
            auto mix = [](double under, double over, double coverage) {
                return under * (1.0 - coverage) + over * coverage;
            };
            double r = mix(red(background), red(track), trackCoverage);
            double g = mix(green(background), green(track), trackCoverage);
            double b = mix(blue(background), blue(track), trackCoverage);
            r = mix(r, red(filled), wedge);
            g = mix(g, green(filled), wedge);
            b = mix(b, blue(filled), wedge);

            // A 32-bit DIB is 0x00RRGGBB, which is the opposite byte order to
            // COLORREF - hence the channels going in this way round.
            pixels[size_t(py) * size_t(size) + size_t(px)] =
                (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        }
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    // Negative, so the rows go top down and match the order they were filled in.
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, x, y, UINT(size), UINT(size), 0, 0, 0, UINT(size), pixels.data(), &info,
                      DIB_RGB_COLORS);
}

}  // namespace

PendingPane* PendingPane::create(HWND parent, int id) {
    ui::registerClass(kClassName, proc);
    auto* pane = new PendingPane();
    pane->window_ =
        CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VSCROLL, 0, 0, 0, 0, parent,
                        HMENU(intptr_t(id)), GetModuleHandleW(nullptr), pane);
    return pane;
}

LRESULT CALLBACK PendingPane::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(create->lpCreateParams));
        reinterpret_cast<PendingPane*>(create->lpCreateParams)->window_ = window;
    }
    auto* pane = reinterpret_cast<PendingPane*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!pane) return DefWindowProcW(window, message, w, l);
    return pane->handle(message, w, l);
}

int PendingPane::rowHeight() const { return ui::scaled(window_, kRowHeight); }

int PendingPane::preferredWidth() const { return ui::scaled(window_, 320); }

void PendingPane::refresh() {
    if (!window_) return;
    layout();
    InvalidateRect(window_, nullptr, FALSE);
}

void PendingPane::layout() {
    if (!window_ || !model_) return;
    RECT client;
    GetClientRect(window_, &client);
    contentHeight_ = int(model_->size()) * rowHeight();

    const int viewport =
        (std::max)(0, int(client.bottom) - ui::scaled(window_, kHeaderHeight + kFooterHeight));
    scrollY_ = (std::max)(0, (std::min)(scrollY_, (std::max)(0, contentHeight_ - viewport)));

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = (std::max)(0, contentHeight_ - 1);
    info.nPage = UINT(viewport);
    info.nPos = scrollY_;
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
}

int PendingPane::hitTestUndo(int x, int y) const {
    for (size_t i = 0; i < undoButtons_.size(); ++i) {
        const RECT& b = undoButtons_[i];
        if (b.right <= b.left) continue;
        if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) return int(i);
    }
    return -1;
}

void PendingPane::paint(HDC target) {
    RECT client;
    GetClientRect(window_, &client);
    const int width = client.right;
    const int height = client.bottom;
    HDC dc = buffer_.begin(target, width, height);
    if (!dc || !model_) return;

    const ui::Theme& t = ui::theme();
    ui::fill(dc, client, t.paneBackground);

    const int header = ui::scaled(window_, kHeaderHeight);
    const int footer = ui::scaled(window_, kFooterHeight);
    const int pad = ui::scaled(window_, 10);
    HFONT bodyFont = ui::font(window_, ui::FontRole::Body);
    HFONT smallFont = ui::font(window_, ui::FontRole::Small);

    RECT headerRect{0, 0, width, header};
    ui::drawText(dc, L"Pending Deletes", RECT{pad, 0, width - pad, header}, t.text,
                 ui::font(window_, ui::FontRole::Header), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ui::fill(dc, RECT{0, header - 1, width, header}, t.separator);
    (void)headerRect;

    const auto& items = model_->items();
    undoButtons_.assign(items.size(), RECT{});

    const int row = rowHeight();
    const int listTop = header;
    const int listBottom = height - footer;
    const int first = (std::max)(0, scrollY_ / row);
    const int last = (std::min)(int(items.size()), (scrollY_ + listBottom - listTop) / row + 2);

    HRGN clip = CreateRectRgn(0, listTop, width, listBottom);
    SelectClipRgn(dc, clip);

    for (int i = first; i < last; ++i) {
        const diskscan::PendingDeletes::Item& item = items[size_t(i)];
        const int y = listTop + i * row - scrollY_;
        RECT rowRect{0, y, width, y + row};

        COLORREF background = t.paneBackground;
        POINT cursor;
        GetCursorPos(&cursor);
        ScreenToClient(window_, &cursor);
        if (PtInRect(&rowRect, cursor)) {
            ui::fill(dc, rowRect, t.hover);
            background = t.hover;
        }

        // Blue while there is time, red as it runs out, crossing over around
        // half way. The curve is the model's, so the three platforms agree.
        const double urgency = item.urgency();
        auto lerp = [urgency](int calm, int urgent) {
            return int(calm + (urgent - calm) * urgency);
        };
        const COLORREF wedge = RGB(lerp(GetRValue(t.selection), 0xd0),
                                   lerp(GetGValue(t.selection), 0x45),
                                   lerp(GetBValue(t.selection), 0x37));
        drawPie(dc, pad, y + (row - ui::scaled(window_, kPieSize)) / 2,
                ui::scaled(window_, kPieSize), item.fraction(), background, wedge, t.separator);

        // The button first, because what is left over is the name's.
        const int buttonWidth = ui::scaled(window_, 74);
        const int buttonHeight = ui::scaled(window_, 22);
        RECT button{width - pad - buttonWidth, y + (row - buttonHeight) / 2, width - pad,
                    y + (row + buttonHeight) / 2};
        undoButtons_[size_t(i)] = button;
        ui::fill(dc, button, hoveredUndo_ == i ? t.hover : t.windowBackground);
        ui::frame(dc, button, t.separator, 1);
        ui::drawText(dc, L"Undelete", button, t.text, smallFont,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const int nameLeft = pad + ui::scaled(window_, kPieSize) + ui::scaled(window_, 8);
        const int sizeWidth = ui::scaled(window_, 68);
        const int nameRight = button.left - ui::scaled(window_, 8) - sizeWidth;
        ui::drawText(dc, item.name, RECT{nameLeft, y, (std::max)(nameLeft + 10, nameRight), y + row},
                     t.text, bodyFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        ui::drawText(dc, ui::formatBytes(item.bytes),
                     RECT{nameRight, y, button.left - ui::scaled(window_, 8), y + row},
                     t.secondaryText, smallFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    // Pinned to the bottom, so it is in the same place whether there is one
    // entry or forty.
    ui::fill(dc, RECT{0, listBottom, width, listBottom + 1}, t.separator);
    const int buttonTop = listBottom + ui::scaled(window_, 8);
    const int buttonBottom = height - ui::scaled(window_, 10);
    const int gap = ui::scaled(window_, 8);
    const int half = (width - pad * 2 - gap) / 2;

    undoAllButton_ = RECT{pad, buttonTop, pad + half, buttonBottom};
    ui::fill(dc, undoAllButton_, hoveringUndoAll_ ? t.hover : t.windowBackground);
    ui::frame(dc, undoAllButton_, t.separator, 1);
    ui::drawText(dc, L"Undelete All", undoAllButton_, t.text, bodyFont,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Red, because it is the one thing in this pane that cannot be undone.
    deleteNowButton_ = RECT{pad + half + gap, buttonTop, width - pad, buttonBottom};
    ui::fill(dc, deleteNowButton_,
             hoveringDeleteNow_ ? RGB(0xc0, 0x39, 0x2b) : RGB(0xd0, 0x45, 0x37));
    ui::drawText(dc, L"Delete Now", deleteNowButton_, RGB(0xff, 0xff, 0xff), bodyFont,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    buffer_.blit(target, width, height);
}

LRESULT PendingPane::handle(UINT message, WPARAM w, LPARAM l) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window_, &ps);
            paint(dc);
            EndPaint(window_, &ps);
            return 0;
        }
        case WM_SIZE:
            layout();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            const int undo = hitTestUndo(x, y);
            const bool overAll = PtInRect(&undoAllButton_, POINT{x, y}) != 0;
            const bool overDelete = PtInRect(&deleteNowButton_, POINT{x, y}) != 0;
            if (undo != hoveredUndo_ || overAll != hoveringUndoAll_ ||
                overDelete != hoveringDeleteNow_) {
                hoveredUndo_ = undo;
                hoveringUndoAll_ = overAll;
                hoveringDeleteNow_ = overDelete;
                SetCursor(LoadCursorW(
                    nullptr, (undo >= 0 || overAll || overDelete) ? IDC_HAND : IDC_ARROW));
                TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window_, 0};
                TrackMouseEvent(&track);
            }
            // The rows highlight under the pointer, so any movement repaints.
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            hoveredUndo_ = -1;
            hoveringUndoAll_ = false;
            hoveringDeleteNow_ = false;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (hoveredUndo_ >= 0 || hoveringUndoAll_ || hoveringDeleteNow_) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return 1;
            }
            break;
        case WM_LBUTTONDOWN: {
            if (!model_) return 0;
            const int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            if (PtInRect(&undoAllButton_, POINT{x, y})) {
                model_->undoAll();
                refresh();
                return 0;
            }
            if (PtInRect(&deleteNowButton_, POINT{x, y})) {
                if (onDeleteNow) onDeleteNow();
                return 0;
            }
            const int undo = hitTestUndo(x, y);
            if (undo >= 0) {
                model_->undo(size_t(undo));
                refresh();
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            scrollY_ -= GET_WHEEL_DELTA_WPARAM(w) * ui::scaled(window_, 60) / WHEEL_DELTA;
            layout();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(window_, SB_VERT, &info);
            switch (LOWORD(w)) {
                case SB_LINEUP: scrollY_ -= rowHeight(); break;
                case SB_LINEDOWN: scrollY_ += rowHeight(); break;
                case SB_PAGEUP: scrollY_ -= int(info.nPage); break;
                case SB_PAGEDOWN: scrollY_ += int(info.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: scrollY_ = info.nTrackPos; break;
                default: break;
            }
            layout();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(window_, message, w, l);
}

}  // namespace app
