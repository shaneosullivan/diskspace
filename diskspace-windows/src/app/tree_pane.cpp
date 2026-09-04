#include "tree_pane.h"

#include <windowsx.h>

#include <algorithm>
#include <iterator>

#include "../core/file_kind.h"
#include "preferences.h"
#include "resource.h"

namespace app {
namespace {

constexpr const wchar_t* kClassName = L"DiskSpaceTreePane";
constexpr int kRowHeight = 18;
constexpr int kHeaderHeight = 24;
constexpr int kIndentPerLevel = 13;
constexpr int kSizeColumnWidth = 118;
constexpr int kSwatchSize = 10;
constexpr int kHeaderButtonSize = 16;

/// Draws a disclosure chevron. Two lines rather than a glyph, so it stays crisp
/// at every scaling factor without shipping an icon per size.
void drawChevron(HDC dc, const RECT& box, bool expanded, COLORREF colour) {
    HPEN pen = CreatePen(PS_SOLID, (std::max)(1, int(box.right - box.left) / 8), colour);
    HPEN previous = HPEN(SelectObject(dc, pen));
    int cx = (box.left + box.right) / 2;
    int cy = (box.top + box.bottom) / 2;
    int arm = (box.right - box.left) / 4;
    POINT points[3];
    if (expanded) {
        points[0] = {cx - arm, cy - arm / 2};
        points[1] = {cx, cy + arm - arm / 2};
        points[2] = {cx + arm, cy - arm / 2};
    } else {
        points[0] = {cx - arm / 2, cy - arm};
        points[1] = {cx + arm - arm / 2, cy};
        points[2] = {cx - arm / 2, cy + arm};
    }
    Polyline(dc, points, 3);
    SelectObject(dc, previous);
    DeleteObject(pen);
}

}  // namespace

TreePane* TreePane::create(HWND parent, int id) {
    ui::registerClass(kClassName, proc);
    auto* pane = new TreePane();
    pane->window_ = CreateWindowExW(0, kClassName, L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL, 0, 0, 0, 0,
                                    parent, HMENU(intptr_t(id)), GetModuleHandleW(nullptr), pane);
    return pane;
}

LRESULT CALLBACK TreePane::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(create->lpCreateParams));
        reinterpret_cast<TreePane*>(create->lpCreateParams)->window_ = window;
    }
    auto* pane = reinterpret_cast<TreePane*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!pane) return DefWindowProcW(window, message, w, l);
    return pane->handle(message, w, l);
}

int TreePane::rowHeight() const {
    return ui::scaled(window_, Preferences::shared().smallFontFiles() ? kRowHeight - 2 : kRowHeight);
}
int TreePane::headerHeight() const { return ui::scaled(window_, kHeaderHeight); }
int TreePane::sizeColumnWidth() const { return ui::scaled(window_, kSizeColumnWidth); }

void TreePane::setArena(const NodeArena* arena, const std::wstring& rootLabel) {
    arena_ = arena;
    rootLabel_ = rootLabel;
    rows_.clear();
    expanded_.clear();
    sortedCache_.clear();
    selected_ = -1;
    selection_.clear();
    anchor_ = -1;
    scrollY_ = 0;
    scrollX_ = 0;
    didExpandRoot_ = false;
    InvalidateRect(window_, nullptr, FALSE);
}

void TreePane::setMetric(SizeMetric metric) {
    metric_ = metric;
    sortedCache_.clear();
    refresh();
}

void TreePane::setGeneration(int generation) { generation_ = generation; }

void TreePane::setSizeColumnTitle(const std::wstring& title) {
    sizeTitle_ = title;
    InvalidateRect(window_, nullptr, FALSE);
}

void TreePane::resetExpansion() {
    expanded_.clear();
    sortedCache_.clear();
    didExpandRoot_ = false;
    selected_ = -1;
    selection_.clear();
    anchor_ = -1;
    scrollY_ = 0;
}

void TreePane::collapseToRoot() {
    expanded_.clear();
    didExpandRoot_ = false;
    scrollY_ = 0;
    refresh();
}

const std::vector<int32_t>& TreePane::sortedChildren(int32_t node) {
    auto it = sortedCache_.find(node);
    if (it != sortedCache_.end()) return it->second;
    std::vector<int32_t> kids;
    if (arena_) {
        auto range = arena_->children(size_t(uint32_t(node)));
        kids.reserve(range.size());
        for (size_t c = range.begin; c < range.end; ++c) kids.push_back(int32_t(c));
        const NodeArena* arena = arena_;
        SizeMetric metric = metric_;
        std::sort(kids.begin(), kids.end(), [arena, metric](int32_t a, int32_t b) {
            return arena->size(metric, size_t(uint32_t(a))) >
                   arena->size(metric, size_t(uint32_t(b)));
        });
    }
    return sortedCache_.emplace(node, std::move(kids)).first->second;
}

void TreePane::appendRows(int32_t node, int depth) {
    const std::vector<int32_t>& kids = sortedChildren(node);
    for (int32_t child : kids) {
        size_t index = size_t(uint32_t(child));
        // Deleted, or waiting in the pending list. Either way it is gone as far
        // as this list is concerned; undoing puts the row straight back.
        if ((arena_->flags[index] & diskscan::NodeFlags::IsRemoved) != 0) continue;
        bool expandable = arena_->isDirectory(index) && !arena_->children(index).empty();
        rows_.push_back({child, depth, expandable});
        if (expandable && expanded_.count(child)) appendRows(child, depth + 1);
    }
}

void TreePane::rebuildRows() {
    rows_.clear();
    if (!arena_ || arena_->count() == 0) return;
    bool rootExpandable = !arena_->children(0).empty();
    rows_.push_back({0, 0, rootExpandable});
    // Open the root as soon as it exists, so the first thing on screen is the
    // top-level breakdown rather than a single collapsed row.
    if (!didExpandRoot_ && rootExpandable) {
        expanded_.insert(0);
        didExpandRoot_ = true;
    }
    if (expanded_.count(0)) appendRows(0, 1);

    // A row that has just gone cannot stay selected, or the next Delete would
    // queue something already deleted and try to bin a path that is not there.
    for (auto it = selection_.begin(); it != selection_.end();) {
        const size_t node = size_t(uint32_t(*it));
        const bool gone = node >= arena_->count() ||
                          (arena_->flags[node] & diskscan::NodeFlags::IsRemoved) != 0;
        it = gone ? selection_.erase(it) : std::next(it);
    }
    if (anchor_ >= 0 && !selection_.count(anchor_)) anchor_ = -1;
    const size_t current = size_t(uint32_t(selected_));
    if (selected_ >= 0 && (current >= arena_->count() ||
                           (arena_->flags[current] & diskscan::NodeFlags::IsRemoved) != 0)) {
        selected_ = -1;
    }
}

void TreePane::refresh() {
    if (cacheGeneration_ != generation_ || cacheMetric_ != metric_) {
        sortedCache_.clear();
        cacheGeneration_ = generation_;
        cacheMetric_ = metric_;
    }

    // Where the topmost selected row is sitting, read before the sort moves it.
    // Only if it is on screen: a selection the user has deliberately scrolled
    // away from has no place worth keeping, and dragging it back would fight
    // them for the scrollbar.
    RECT client;
    GetClientRect(window_, &client);
    const int viewport = client.bottom - headerHeight();
    int32_t anchor = -1;
    int anchorOffset = 0;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (!selection_.count(rows_[i].node)) continue;
        const int y = int(i) * rowHeight();
        if (y + rowHeight() > scrollY_ && y < scrollY_ + viewport) {
            anchor = rows_[i].node;
            anchorOffset = y - scrollY_;
        }
        break;
    }

    rebuildRows();
    keepAnchorInView(anchor, anchorOffset);
    updateScrollBars();
    InvalidateRect(window_, nullptr, FALSE);
}

void TreePane::keepAnchorInView(int32_t node, int offset) {
    if (node < 0 || !selection_.count(node)) return;
    RECT client;
    GetClientRect(window_, &client);
    const int viewport = client.bottom - headerHeight();
    if (viewport <= 0) return;

    int index = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].node == node) {
            index = int(i);
            break;
        }
    }
    if (index < 0) return;

    const int y = index * rowHeight();
    // Still on screen: leave the list alone. Scrolling underneath someone who
    // can already see what they picked is its own kind of unhelpful.
    if (y >= scrollY_ && y + rowHeight() <= scrollY_ + viewport) return;

    const int maximum = (std::max)(0, int(rows_.size()) * rowHeight() - viewport);
    scrollY_ = (std::max)(0, (std::min)(y - offset, maximum));
}

std::wstring TreePane::nameOf(size_t node) const {
    if (node == 0) return rootLabel_;
    return arena_->name(node);
}

void TreePane::toggleExpansion(int32_t node) {
    if (expanded_.count(node)) {
        expanded_.erase(node);
    } else {
        expanded_.insert(node);
    }
    refresh();
}

void TreePane::reveal(int32_t target) {
    if (!arena_ || target < 0 || size_t(uint32_t(target)) >= arena_->count()) return;
    std::vector<int32_t> chain;
    size_t cur = size_t(uint32_t(target));
    while (cur > 0) {
        cur = size_t(uint32_t(arena_->parent[cur]));
        chain.push_back(int32_t(cur));
    }
    for (int32_t node : chain) expanded_.insert(node);
    didExpandRoot_ = true;
    refresh();
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].node == target) {
            select(int(i), true);
            return;
        }
    }
}

void TreePane::selectOnly(int32_t node) {
    selection_.clear();
    selection_.insert(node);
    selected_ = node;
    anchor_ = node;
}

void TreePane::select(int rowIndex, bool scrollIntoView) {
    if (rowIndex < 0 || rowIndex >= int(rows_.size())) return;
    selectOnly(rows_[size_t(rowIndex)].node);
    if (scrollIntoView) scrollToRow(rowIndex, true);
    InvalidateRect(window_, nullptr, FALSE);
    if (onSelect) onSelect(selected_);
}

void TreePane::selectRangeTo(int rowIndex) {
    if (rowIndex < 0 || rowIndex >= int(rows_.size()) || !arena_ || anchor_ < 0) {
        select(rowIndex, false);
        return;
    }

    // Only rows beside the anchor: the same parent. Everything between them in
    // the list that belongs to some other folder - a subtree that happens to be
    // expanded - is passed over rather than swept up, because the user cannot see
    // what would relate those rows to the two they picked.
    const int32_t node = rows_[size_t(rowIndex)].node;
    const int32_t parent = arena_->parent[size_t(uint32_t(anchor_))];
    if (arena_->parent[size_t(uint32_t(node))] != parent) {
        select(rowIndex, false);
        return;
    }

    int first = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].node == anchor_) {
            first = int(i);
            break;
        }
    }
    if (first < 0) {
        select(rowIndex, false);
        return;
    }
    int last = rowIndex;
    if (first > last) std::swap(first, last);

    selection_.clear();
    for (int i = first; i <= last; ++i) {
        const int32_t candidate = rows_[size_t(i)].node;
        // Never the scan root. Its parent is itself, so a comparison by parent
        // alone reads it as a sibling of everything at the top level - and it is
        // the one row that can never be deleted.
        if (candidate == 0) continue;
        if (arena_->parent[size_t(uint32_t(candidate))] == parent) selection_.insert(candidate);
    }
    // The anchor stays where it was, so dragging the other end of the range keeps
    // measuring from the same place.
    selected_ = node;
    InvalidateRect(window_, nullptr, FALSE);
    if (onSelect) onSelect(selected_);
}

void TreePane::scrollToRow(int rowIndex, bool centre) {
    RECT client;
    GetClientRect(window_, &client);
    int viewport = client.bottom - headerHeight();
    if (viewport <= 0) return;
    int y = rowIndex * rowHeight();
    if (centre) {
        // Scrolling the minimum distance leaves a row clicked from the map pinned
        // to the very bottom edge — visible, but with none of the surrounding
        // folders that give it context.
        scrollY_ = y - viewport / 2 + rowHeight() / 2;
    } else if (y < scrollY_) {
        scrollY_ = y;
    } else if (y + rowHeight() > scrollY_ + viewport) {
        scrollY_ = y + rowHeight() - viewport;
    }
    int maximum = (std::max)(0, int(rows_.size()) * rowHeight() - viewport);
    scrollY_ = (std::max)(0, (std::min)(scrollY_, maximum));
    updateScrollBars();
}

int TreePane::rowAt(int y) const {
    int top = headerHeight();
    if (y < top) return -1;
    int index = (y - top + scrollY_) / rowHeight();
    return index >= 0 && index < int(rows_.size()) ? index : -1;
}

int TreePane::contentWidth(HDC dc) {
    if (!arena_) return 0;
    HFONT font = ui::font(window_, Preferences::shared().smallFontFiles() ? ui::FontRole::Small
                                                                         : ui::FontRole::Body);
    int widest = 0;
    int indent = ui::scaled(window_, kIndentPerLevel);
    // Only the rows that exist are measured, which is bounded by what is expanded
    // rather than by the size of the tree.
    for (const Row& row : rows_) {
        std::wstring name = nameOf(size_t(uint32_t(row.node)));
        SIZE size = ui::measureText(dc, name, font);
        int width = row.depth * indent + ui::scaled(window_, 40) + size.cx;
        widest = (std::max)(widest, width);
    }
    return widest;
}

void TreePane::updateScrollBars() {
    RECT client;
    GetClientRect(window_, &client);
    int viewport = (std::max)(0L, client.bottom - headerHeight());

    SCROLLINFO vertical{};
    vertical.cbSize = sizeof(vertical);
    vertical.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    vertical.nMin = 0;
    vertical.nMax = (std::max)(0, int(rows_.size()) * rowHeight() - 1);
    vertical.nPage = UINT(viewport);
    int maximumY = (std::max)(0, int(rows_.size()) * rowHeight() - viewport);
    scrollY_ = (std::max)(0, (std::min)(scrollY_, maximumY));
    vertical.nPos = scrollY_;
    SetScrollInfo(window_, SB_VERT, &vertical, TRUE);

    int nameWidth = (std::max)(0L, client.right - sizeColumnWidth());
    SCROLLINFO horizontal{};
    horizontal.cbSize = sizeof(horizontal);
    horizontal.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    horizontal.nMin = 0;
    horizontal.nMax = (std::max)(0, contentWidth_ - 1);
    horizontal.nPage = UINT(nameWidth);
    int maximumX = (std::max)(0, contentWidth_ - nameWidth);
    scrollX_ = (std::max)(0, (std::min)(scrollX_, maximumX));
    horizontal.nPos = scrollX_;
    SetScrollInfo(window_, SB_HORZ, &horizontal, TRUE);
}

// MARK: - Painting

void TreePane::paintHeader(HDC dc, const RECT& bounds) {
    const ui::Theme& t = ui::theme();
    ui::fill(dc, bounds, t.headerBackground);
    RECT edge{bounds.left, bounds.bottom - 1, bounds.right, bounds.bottom};
    ui::fill(dc, edge, t.separator);

    int button = ui::scaled(window_, kHeaderButtonSize);
    int pad = ui::scaled(window_, 5);
    int top = bounds.top + (bounds.bottom - bounds.top - button) / 2;

    RECT sidePanel{bounds.left + pad, top, bounds.left + pad + button, top + button};
    RECT collapse{sidePanel.right + ui::scaled(window_, 6), top,
                  sidePanel.right + ui::scaled(window_, 6) + button, top + button};
    if (hoveredHeaderButton_ == 0) ui::fill(dc, sidePanel, t.hover);
    if (hoveredHeaderButton_ == 1) ui::fill(dc, collapse, t.hover);
    // The same artwork as the macOS app, tinted to the header colour so it reads
    // in either theme. The arrow points the way the panel will move.
    int glyphSize = ui::scaled(window_, 15);
    ui::drawTemplateImage(dc, sidePanelVisible ? IDB_SIDE_HIDE : IDB_SIDE_SHOW, sidePanel,
                          glyphSize, t.headerText);
    ui::drawTemplateImage(dc, IDB_COLLAPSE, collapse, glyphSize, t.headerText);

    HFONT font = ui::font(window_, ui::FontRole::Header);
    RECT nameTitle{collapse.right + ui::scaled(window_, 8), bounds.top,
                   bounds.right - sizeColumnWidth(), bounds.bottom - 1};
    ui::drawText(dc, L"Name", nameTitle, t.headerText, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT sizeTitle{bounds.right - sizeColumnWidth(), bounds.top, bounds.right - ui::scaled(window_, 8),
                   bounds.bottom - 1};
    ui::drawText(dc, sizeTitle_, sizeTitle, t.headerText, font,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT divider{sizeTitle.left - ui::scaled(window_, 6), bounds.top + 3,
                 sizeTitle.left - ui::scaled(window_, 6) + 1, bounds.bottom - 4};
    ui::fill(dc, divider, t.separator);
}

void TreePane::paintRow(HDC dc, const RECT& bounds, const Row& row, bool selected, bool alternate) {
    const ui::Theme& t = ui::theme();
    ui::fill(dc, bounds, selected ? t.selection : (alternate ? t.alternateRow : t.paneBackground));

    const size_t node = size_t(uint32_t(row.node));
    const bool small = Preferences::shared().smallFontFiles();
    HFONT font = ui::font(window_, small ? ui::FontRole::Small : ui::FontRole::Body);
    COLORREF text = selected ? t.selectionText : t.text;

    const int indent = ui::scaled(window_, kIndentPerLevel);
    const int chevron = ui::scaled(window_, 12);
    const int swatch = ui::scaled(window_, kSwatchSize);
    const int sizeWidth = sizeColumnWidth();

    // Everything left of the size column scrolls; the size column never does.
    RECT nameArea{bounds.left, bounds.top, bounds.right - sizeWidth, bounds.bottom};
    HRGN clip = CreateRectRgn(nameArea.left, nameArea.top, nameArea.right, nameArea.bottom);
    SelectClipRgn(dc, clip);

    int x = bounds.left + ui::scaled(window_, 4) + row.depth * indent - scrollX_;
    if (row.expandable) {
        RECT box{x, bounds.top, x + chevron, bounds.bottom};
        drawChevron(dc, box, expanded_.count(row.node) != 0,
                    selected ? t.selectionText : t.secondaryText);
    }
    x += chevron + ui::scaled(window_, 2);

    if (palette_) {
        int top = bounds.top + ((bounds.bottom - bounds.top) - swatch) / 2;
        RECT block{x, top, x + swatch, top + swatch};
        ui::fill(dc, block, palette_->gdiColour(arena_->kind[node]));
    }
    x += swatch + ui::scaled(window_, 5);

    RECT nameRect{x, bounds.top, nameArea.right - ui::scaled(window_, 4), bounds.bottom};
    ui::drawText(dc, nameOf(node), nameRect, text, font,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    int64_t bytes = displaySize ? displaySize(node) : arena_->size(metric_, node);
    RECT sizeRect{bounds.right - sizeWidth, bounds.top, bounds.right - ui::scaled(window_, 8),
                  bounds.bottom};
    ui::drawText(dc, ui::formatBytes(bytes), sizeRect, selected ? t.selectionText : t.text, font,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void TreePane::paint(HDC target) {
    RECT client;
    GetClientRect(window_, &client);
    int width = client.right;
    int height = client.bottom;
    HDC dc = buffer_.begin(target, width, height);
    if (!dc) return;

    const ui::Theme& t = ui::theme();
    ui::fill(dc, client, t.paneBackground);

    RECT header{0, 0, width, headerHeight()};
    paintHeader(dc, header);

    // Measuring names needs a device context, so the widest row is only known
    // here. When it changes the scroll range has to be re-applied, or the
    // horizontal scrollbar keeps a range from before the rows existed.
    if (arena_) {
        int measured = contentWidth(dc);
        if (measured != contentWidth_) {
            contentWidth_ = measured;
            updateScrollBars();
        }
    }

    int rowH = rowHeight();
    int first = scrollY_ / rowH;
    int y = headerHeight() - (scrollY_ % rowH);
    for (int i = first; i < int(rows_.size()) && y < height; ++i, y += rowH) {
        RECT bounds{0, y, width, y + rowH};
        paintRow(dc, bounds, rows_[size_t(i)],
                 selection_.count(rows_[size_t(i)].node) != 0, (i & 1) != 0);
    }

    // The pinned column's left edge, drawn last so it sits above the rows.
    RECT divider{width - sizeColumnWidth() - ui::scaled(window_, 6), headerHeight(),
                 width - sizeColumnWidth() - ui::scaled(window_, 6) + 1, height};
    ui::fill(dc, divider, t.separator);

    buffer_.blit(target, width, height);
}

// MARK: - Messages

LRESULT TreePane::handle(UINT message, WPARAM w, LPARAM l) {
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
            updateScrollBars();
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(window_, SB_VERT, &info);
            int position = scrollY_;
            switch (LOWORD(w)) {
                case SB_LINEUP: position -= rowHeight(); break;
                case SB_LINEDOWN: position += rowHeight(); break;
                case SB_PAGEUP: position -= int(info.nPage); break;
                case SB_PAGEDOWN: position += int(info.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: position = info.nTrackPos; break;
                default: break;
            }
            scrollY_ = position;
            updateScrollBars();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_HSCROLL: {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(window_, SB_HORZ, &info);
            int position = scrollX_;
            int step = ui::scaled(window_, 24);
            switch (LOWORD(w)) {
                case SB_LINELEFT: position -= step; break;
                case SB_LINERIGHT: position += step; break;
                case SB_PAGELEFT: position -= int(info.nPage); break;
                case SB_PAGERIGHT: position += int(info.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: position = info.nTrackPos; break;
                default: break;
            }
            scrollX_ = position;
            updateScrollBars();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            scrollY_ -= delta * int(lines) * rowHeight() / WHEEL_DELTA;
            updateScrollBars();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            int button = -1;
            if (y < headerHeight()) {
                int size = ui::scaled(window_, kHeaderButtonSize);
                int pad = ui::scaled(window_, 5);
                if (x >= pad && x < pad + size) button = 0;
                else if (x >= pad + size + ui::scaled(window_, 6) &&
                         x < pad + 2 * size + ui::scaled(window_, 6)) {
                    button = 1;
                }
            }
            if (button != hoveredHeaderButton_) {
                hoveredHeaderButton_ = button;
                TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window_, 0};
                TrackMouseEvent(&track);
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hoveredHeaderButton_ != -1) {
                hoveredHeaderButton_ = -1;
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(window_);
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            if (y < headerHeight()) {
                if (hoveredHeaderButton_ == 0 && onToggleSidePanel) onToggleSidePanel();
                if (hoveredHeaderButton_ == 1 && onCollapseAll) onCollapseAll();
                return 0;
            }
            int index = rowAt(y);
            if (index < 0) return 0;
            const Row& row = rows_[size_t(index)];
            int indent = ui::scaled(window_, kIndentPerLevel);
            int chevronLeft = ui::scaled(window_, 4) + row.depth * indent - scrollX_;
            int chevronRight = chevronLeft + ui::scaled(window_, 12);
            if (row.expandable && x >= chevronLeft && x < chevronRight) {
                toggleExpansion(row.node);
                return 0;
            }
            // Control adds to the selection, shift extends from the anchor, and
            // a plain click replaces it.
            if (GetKeyState(VK_CONTROL) < 0) {
                const int32_t node = row.node;
                if (selection_.count(node)) {
                    selection_.erase(node);
                    if (selected_ == node) {
                        selected_ = selection_.empty() ? -1 : *selection_.begin();
                    }
                } else {
                    selection_.insert(node);
                    selected_ = node;
                }
                anchor_ = node;
                InvalidateRect(window_, nullptr, FALSE);
                if (onSelect && selected_ >= 0) onSelect(selected_);
            } else if (GetKeyState(VK_SHIFT) < 0) {
                selectRangeTo(index);
            } else {
                select(index, false);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int index = rowAt(GET_Y_LPARAM(l));
            if (index >= 0 && rows_[size_t(index)].expandable) {
                toggleExpansion(rows_[size_t(index)].node);
            }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            int index = rowAt(GET_Y_LPARAM(l));
            // A right click inside an existing selection acts on all of it. Only
            // one landing outside narrows the selection to the row under the
            // pointer, which is what every list does.
            if (index >= 0 && !selection_.count(rows_[size_t(index)].node)) {
                select(index, false);
            }
            POINT screen{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(window_, &screen);
            SendMessageW(GetParent(window_), WM_CONTEXTMENU, WPARAM(window_),
                         MAKELPARAM(screen.x, screen.y));
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN: {
            int current = -1;
            for (size_t i = 0; i < rows_.size(); ++i) {
                if (rows_[i].node == selected_) {
                    current = int(i);
                    break;
                }
            }
            RECT client;
            GetClientRect(window_, &client);
            int page = (std::max)(1L, (client.bottom - headerHeight()) / rowHeight());
            switch (w) {
                case VK_DOWN: select(current + 1, true); return 0;
                case VK_UP: if (current > 0) select(current - 1, true); return 0;
                case VK_PRIOR: select((std::max)(0, current - page), true); return 0;
                case VK_NEXT: select((std::min)(int(rows_.size()) - 1, current + page), true); return 0;
                case VK_HOME: select(0, true); return 0;
                case VK_END: select(int(rows_.size()) - 1, true); return 0;
                case VK_RIGHT:
                    if (current >= 0 && rows_[size_t(current)].expandable &&
                        !expanded_.count(rows_[size_t(current)].node)) {
                        toggleExpansion(rows_[size_t(current)].node);
                    }
                    return 0;
                case VK_LEFT:
                    if (current >= 0) {
                        const Row& row = rows_[size_t(current)];
                        if (row.expandable && expanded_.count(row.node)) {
                            toggleExpansion(row.node);
                        } else if (row.node != 0) {
                            // Already shut, so the useful move is out to the folder
                            // holding it rather than nothing at all.
                            reveal(arena_->parent[size_t(uint32_t(row.node))]);
                        }
                    }
                    return 0;
                default: break;
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(window_, message, w, l);
}

}  // namespace app
