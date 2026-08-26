#include "kind_pane.h"

#include <windowsx.h>

#include <algorithm>

#include "../core/file_kind.h"
#include "preferences.h"

namespace app {
namespace {

constexpr const wchar_t* kClassName = L"DiskSpaceKindPane";
constexpr int kRowHeight = 18;
constexpr int kHeaderHeight = 24;
constexpr int kColourWidth = 20;
constexpr int kKindWidth = 132;
constexpr int kSizeWidth = 74;
constexpr int kFilesWidth = 68;

}  // namespace

KindPane* KindPane::create(HWND parent, int id) {
    ui::registerClass(kClassName, proc);
    auto* pane = new KindPane();
    pane->window_ =
        CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 0, 0, parent,
                        HMENU(intptr_t(id)), GetModuleHandleW(nullptr), pane);
    return pane;
}

LRESULT CALLBACK KindPane::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(create->lpCreateParams));
        reinterpret_cast<KindPane*>(create->lpCreateParams)->window_ = window;
    }
    auto* pane = reinterpret_cast<KindPane*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!pane) return DefWindowProcW(window, message, w, l);
    return pane->handle(message, w, l);
}

int KindPane::rowHeight() const {
    return ui::scaled(window_, Preferences::shared().smallFontKinds() ? kRowHeight - 2 : kRowHeight);
}
int KindPane::headerHeight() const { return ui::scaled(window_, kHeaderHeight); }

int KindPane::preferredWidth() const {
    return ui::scaled(window_, kColourWidth + kKindWidth + kSizeWidth + kFilesWidth + 24);
}

void KindPane::columns(int width, RECT* colour, RECT* kind, RECT* size, RECT* files) const {
    int x = 0;
    int colourWidth = ui::scaled(window_, kColourWidth);
    int sizeWidth = ui::scaled(window_, kSizeWidth);
    int filesWidth = ui::scaled(window_, kFilesWidth);
    // The kind column absorbs whatever width is left, so widening the pane makes
    // long type names readable rather than adding empty space at the edge.
    int kindWidth = (std::max)(ui::scaled(window_, 60), width - colourWidth - sizeWidth - filesWidth);
    *colour = {x, 0, x + colourWidth, 0};
    x += colourWidth;
    *kind = {x, 0, x + kindWidth, 0};
    x += kindWidth;
    *size = {x, 0, x + sizeWidth, 0};
    x += sizeWidth;
    *files = {x, 0, x + filesWidth, 0};
}

void KindPane::refresh() {
    rows_ = scanner_ ? scanner_->kindTotals(metric_, useLogicalSize_)
                     : std::vector<diskscan::TreeScanner::KindTotal>();
    applySort();
    updateScrollBar();
    InvalidateRect(window_, nullptr, FALSE);
}

void KindPane::applySort() {
    using Row = diskscan::TreeScanner::KindTotal;
    switch (sortKey_) {
        case SortKey::Kind:
            std::sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) {
                return diskscan::FileKind::names()[size_t(a.kind)] <
                       diskscan::FileKind::names()[size_t(b.kind)];
            });
            break;
        case SortKey::Files:
            std::sort(rows_.begin(), rows_.end(),
                      [](const Row& a, const Row& b) { return a.count < b.count; });
            break;
        default:
            std::sort(rows_.begin(), rows_.end(),
                      [](const Row& a, const Row& b) { return a.bytes < b.bytes; });
            break;
    }
    if (!ascending_) std::reverse(rows_.begin(), rows_.end());
}

void KindPane::updateScrollBar() {
    RECT client;
    GetClientRect(window_, &client);
    int viewport = (std::max)(0L, client.bottom - headerHeight());
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = (std::max)(0, int(rows_.size()) * rowHeight() - 1);
    info.nPage = UINT(viewport);
    int maximum = (std::max)(0, int(rows_.size()) * rowHeight() - viewport);
    scrollY_ = (std::max)(0, (std::min)(scrollY_, maximum));
    info.nPos = scrollY_;
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
}

void KindPane::paint(HDC target) {
    RECT client;
    GetClientRect(window_, &client);
    int width = client.right;
    int height = client.bottom;
    HDC dc = buffer_.begin(target, width, height);
    if (!dc) return;

    const ui::Theme& t = ui::theme();
    ui::fill(dc, client, t.paneBackground);

    RECT colour, kind, size, files;
    columns(width, &colour, &kind, &size, &files);

    RECT header{0, 0, width, headerHeight()};
    ui::fill(dc, header, t.headerBackground);
    RECT edge{0, header.bottom - 1, width, header.bottom};
    ui::fill(dc, edge, t.separator);
    HFONT headerFont = ui::font(window_, ui::FontRole::Header);
    int pad = ui::scaled(window_, 5);

    auto heading = [&](const RECT& column, const wchar_t* title, SortKey key, UINT align) {
        RECT box{column.left + pad, header.top, column.right - pad, header.bottom - 1};
        std::wstring text = title;
        // The arrow goes in the heading rather than in a separate indicator, so
        // which column is sorted is legible at a glance in a narrow pane.
        if (sortKey_ == key) text += ascending_ ? L"  ↑" : L"  ↓";
        ui::drawText(dc, text, box, t.headerText, headerFont, align | DT_VCENTER | DT_SINGLELINE);
    };
    heading(kind, L"Kind", SortKey::Kind, DT_LEFT);
    heading(size, L"Size", SortKey::Size, DT_RIGHT);
    heading(files, L"Files", SortKey::Files, DT_RIGHT);

    const bool small = Preferences::shared().smallFontKinds();
    HFONT font = ui::font(window_, small ? ui::FontRole::Small : ui::FontRole::Body);
    int rowH = rowHeight();
    int first = scrollY_ / rowH;
    int y = headerHeight() - (scrollY_ % rowH);
    int swatch = ui::scaled(window_, 12);

    for (int i = first; i < int(rows_.size()) && y < height; ++i, y += rowH) {
        const auto& row = rows_[size_t(i)];
        RECT bounds{0, y, width, y + rowH};
        ui::fill(dc, bounds, (i & 1) ? t.alternateRow : t.paneBackground);

        if (palette_) {
            int top = y + (rowH - swatch) / 2;
            RECT block{colour.left + ui::scaled(window_, 3), top,
                       colour.left + ui::scaled(window_, 3) + swatch, top + swatch};
            ui::fill(dc, block, palette_->gdiColour(row.kind));
        }
        RECT box{kind.left + ui::scaled(window_, 2), y, kind.right - pad, y + rowH};
        ui::drawText(dc, diskscan::FileKind::names()[size_t(row.kind)], box, t.text, font,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        box = {size.left, y, size.right - pad, y + rowH};
        ui::drawText(dc, ui::formatBytes(row.bytes), box, t.text, font,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        box = {files.left, y, files.right - pad, y + rowH};
        ui::drawText(dc, ui::formatCount(row.count), box, t.secondaryText, font,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    buffer_.blit(target, width, height);
}

LRESULT KindPane::handle(UINT message, WPARAM w, LPARAM l) {
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
            updateScrollBar();
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
            updateScrollBar();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            scrollY_ -= GET_WHEEL_DELTA_WPARAM(w) * int(lines) * rowHeight() / WHEEL_DELTA;
            updateScrollBar();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int y = GET_Y_LPARAM(l);
            if (y >= headerHeight()) return 0;
            RECT client;
            GetClientRect(window_, &client);
            RECT colour, kind, size, files;
            columns(client.right, &colour, &kind, &size, &files);
            int x = GET_X_LPARAM(l);
            SortKey key = sortKey_;
            if (x >= kind.left && x < kind.right) key = SortKey::Kind;
            else if (x >= size.left && x < size.right) key = SortKey::Size;
            else if (x >= files.left && x < files.right) key = SortKey::Files;
            else return 0;
            // Clicking the column already sorted reverses it; a different column
            // starts at the order that column is usually wanted in.
            if (key == sortKey_) {
                ascending_ = !ascending_;
            } else {
                sortKey_ = key;
                ascending_ = key == SortKey::Kind;
            }
            applySort();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(window_, message, w, l);
}

}  // namespace app
