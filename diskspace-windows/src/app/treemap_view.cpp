#include "treemap_view.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <thread>

#include "preferences.h"

#pragma comment(lib, "comctl32.lib")

namespace app {
namespace {

constexpr const wchar_t* kClassName = L"DiskSpaceTreemapView";
/// Posted from the render thread. wParam is the generation, lParam the image.
constexpr UINT kRenderDone = WM_APP + 10;
constexpr UINT kAnimationTimer = 2;
constexpr double kZoomDurationMs = 320;

/// Blits a 32-bit buffer straight to the device. No intermediate HBITMAP: the
/// pixels already arrive in the layout a BI_RGB device independent bitmap wants.
void blitPixels(HDC dc, const uint32_t* pixels, int width, int height, const RECT& destination) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    // Negative height means top-down, which matches the order the raster wrote in.
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    int previousMode = SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, destination.left, destination.top, destination.right - destination.left,
                  destination.bottom - destination.top, 0, 0, width, height, pixels, &info,
                  DIB_RGB_COLORS, SRCCOPY);
    SetStretchBltMode(dc, previousMode);
}

double eased(double t) {
    // Ease in and out, so the motion starts and ends still rather than snapping.
    return t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;
}

}  // namespace

TreemapView* TreemapView::create(HWND parent, int id) {
    ui::registerClass(kClassName, proc);
    auto* view = new TreemapView();
    view->window_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                                    HMENU(intptr_t(id)), GetModuleHandleW(nullptr), view);
    return view;
}

LRESULT CALLBACK TreemapView::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(create->lpCreateParams));
        reinterpret_cast<TreemapView*>(create->lpCreateParams)->window_ = window;
    }
    auto* view = reinterpret_cast<TreemapView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!view) return DefWindowProcW(window, message, w, l);
    return view->handle(message, w, l);
}

void TreemapView::setArena(const NodeArena* arena) {
    arena_ = arena;
    current_.reset();
    hoveredCell_ = -1;
    highlighted_ = -1;
    zoomStack_ = {{0, RectD{}}};
    transition_.reset();
    InvalidateRect(window_, nullptr, FALSE);
}

void TreemapView::setMetric(SizeMetric metric) {
    options_.metric = metric;
    refresh();
}

void TreemapView::setOptions(const TreemapOptions& options) {
    SizeMetric metric = options_.metric;
    options_ = options;
    options_.metric = metric;
    refresh();
}

void TreemapView::refresh() { requestRender(); }

void TreemapView::resetLivePacing() {
    lastLiveRefresh_ = 0;
    averageRenderCost_ = 0;
    liveInterval_ = kFastInterval;
}

void TreemapView::liveRefresh() {
    ULONGLONG now = GetTickCount64();
    if (lastLiveRefresh_ != 0 &&
        double(now - lastLiveRefresh_) / 1000.0 < liveInterval_) {
        return;
    }
    lastLiveRefresh_ = now;
    requestRender();
}

void TreemapView::recordRenderCost(double seconds) {
    // A rolling average rather than the last sample: one unusually deep frame
    // should not stretch the interval for the rest of the scan.
    averageRenderCost_ = averageRenderCost_ == 0 ? seconds : averageRenderCost_ * 0.7 + seconds * 0.3;
    if (averageRenderCost_ > liveInterval_ * kOverBudget) {
        liveInterval_ = (std::min)(kSlowInterval, averageRenderCost_ / kComfortable);
    } else if (averageRenderCost_ < liveInterval_ * kComfortable * 0.5) {
        liveInterval_ = (std::max)(kFastInterval, liveInterval_ * 0.8);
    }
}

void TreemapView::setHighlightedNode(int32_t node) {
    if (highlighted_ == node) return;
    highlighted_ = node;
    InvalidateRect(window_, nullptr, FALSE);
}

const std::vector<int32_t> TreemapView::zoomChain() const {
    std::vector<int32_t> out;
    for (const ZoomEntry& entry : zoomStack_) out.push_back(entry.node);
    return out;
}

void TreemapView::zoomTo(int32_t node) {
    if (!arena_ || !arena_->isDirectory(size_t(uint32_t(node)))) return;
    if (node == zoomedNode()) return;
    RectD focus{};
    if (current_) {
        auto it = current_->nodeRects.find(node);
        if (it != current_->nodeRects.end()) focus = viewRect(it->second);
    }
    zoomStack_.push_back({node, focus});
    if (Preferences::shared().animatedZooming() && focus.width > 1 && focus.height > 1) {
        beginTransition(focus, true);
    }
    requestRender();
    if (onZoomChanged) onZoomChanged();
}

void TreemapView::zoomOut() {
    if (zoomStack_.size() <= 1) return;
    ZoomEntry leaving = zoomStack_.back();
    zoomStack_.pop_back();
    if (Preferences::shared().animatedZooming() && leaving.rectInParent.width > 1) {
        beginTransition(leaving.rectInParent, false);
    }
    requestRender();
    if (onZoomChanged) onZoomChanged();
}

void TreemapView::resetZoom() {
    zoomStack_ = {{0, RectD{}}};
    transition_.reset();
    requestRender();
    if (onZoomChanged) onZoomChanged();
}

int32_t TreemapView::nodeForCell(int cellIndex) const {
    if (!current_ || cellIndex < 0 || cellIndex >= int(current_->cells.size())) return -1;
    const TreemapCell& cell = current_->cells[size_t(cellIndex)];
    if (cell.node < 0) return cell.node;
    // A pooled cell stands for many items at once, so the thing to select is the
    // folder holding them rather than whichever one lent its colour.
    if (cell.aggregatedCount > 0 && arena_) {
        return arena_->parent[size_t(uint32_t(cell.node))];
    }
    return cell.node;
}

// MARK: - Rendering

void TreemapView::requestRender() {
    if (!arena_ || arena_->count() == 0 || !palette_) return;
    RECT client;
    GetClientRect(window_, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width <= 1 || height <= 1) return;

    if (rendering_) {
        // One queued request is enough: the next render will read whatever the
        // arena holds at that moment, which is newer than anything we could
        // capture now.
        renderQueued_ = true;
        return;
    }
    rendering_ = true;
    renderQueued_ = false;

    int generation = ++renderGeneration_;
    int32_t root = zoomedNode();
    // The root may not have been published yet on a very early tick.
    if (size_t(uint32_t(root)) >= arena_->count()) root = 0;
    const NodeArena* arena = arena_;
    KindPalette palette = *palette_;
    TreemapOptions options = options_;
    HWND window = window_;

    std::thread([arena, root, width, height, palette, options, generation, window] {
        ULONGLONG began = GetTickCount64();
        auto* image = new TreemapImage(Treemap::render(*arena, root, width, height, palette, options));
        double milliseconds = double(GetTickCount64() - began);
        if (!PostMessageW(window, kRenderDone, WPARAM(generation),
                          LPARAM(new std::pair<TreemapImage*, double>(image, milliseconds)))) {
            delete image;
        }
    }).detach();
}

void TreemapView::onRenderComplete(TreemapImage* image, double milliseconds) {
    rendering_ = false;
    current_.reset(image);
    hoveredCell_ = -1;
    recordRenderCost(milliseconds / 1000.0);
    InvalidateRect(window_, nullptr, FALSE);
    if (renderQueued_) requestRender();
}

RectD TreemapView::viewRect(const RectD& rect) const {
    if (!current_ || current_->width == 0 || current_->height == 0) return rect;
    RECT client;
    GetClientRect(window_, &client);
    double sx = double(client.right - client.left) / double(current_->width);
    double sy = double(client.bottom - client.top) / double(current_->height);
    return {rect.x * sx, rect.y * sy, rect.width * sx, rect.height * sy};
}

void TreemapView::beginTransition(const RectD& focus, bool zoomingIn) {
    if (!current_ || current_->pixels.empty()) return;
    auto transition = std::make_unique<Transition>();
    transition->outgoing = current_->pixels;
    transition->outgoingWidth = current_->width;
    transition->outgoingHeight = current_->height;
    transition->focus = focus;
    transition->zoomingIn = zoomingIn;
    transition->start = GetTickCount64();
    transition_ = std::move(transition);
    SetTimer(window_, kAnimationTimer, 16, nullptr);
}

void TreemapView::endTransition() {
    KillTimer(window_, kAnimationTimer);
    transition_.reset();
    InvalidateRect(window_, nullptr, FALSE);
}

void TreemapView::paint(HDC target) {
    RECT client;
    GetClientRect(window_, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    HDC dc = buffer_.begin(target, width, height);
    if (!dc) return;

    ui::fill(dc, client, ui::theme().treemapBackground);

    if (transition_) {
        double raw = (std::min)(1.0, double(GetTickCount64() - transition_->start) / kZoomDurationMs);
        double progress = eased(raw);
        const RectD& focus = transition_->focus;
        // Zooming in, the outgoing map grows until the focused rectangle fills the
        // view; zooming out, it shrinks back into the rectangle it came from.
        double scale;
        double centreX, centreY;
        if (transition_->zoomingIn) {
            double full = (std::max)(double(width) / (std::max)(focus.width, 1.0),
                                     double(height) / (std::max)(focus.height, 1.0));
            full = (std::max)(full, 1.45);
            scale = 1 + (full - 1) * progress;
            centreX = focus.midX() + (width / 2.0 - focus.midX()) * progress;
            centreY = focus.midY() + (height / 2.0 - focus.midY()) * progress;
        } else {
            double full = (std::max)(double(width) / (std::max)(focus.width, 1.0),
                                     double(height) / (std::max)(focus.height, 1.0));
            full = (std::max)(full, 1.45);
            scale = full + (1 - full) * progress;
            centreX = width / 2.0 + (focus.midX() - width / 2.0) * progress;
            centreY = height / 2.0 + (focus.midY() - height / 2.0) * progress;
        }
        RECT destination;
        destination.left = LONG(std::lround(centreX - width * scale / 2));
        destination.top = LONG(std::lround(centreY - height * scale / 2));
        destination.right = destination.left + LONG(std::lround(width * scale));
        destination.bottom = destination.top + LONG(std::lround(height * scale));
        blitPixels(dc, transition_->outgoing.data(), transition_->outgoingWidth,
                   transition_->outgoingHeight, destination);
        buffer_.blit(target, width, height);
        if (raw >= 1) endTransition();
        return;
    }

    if (!current_ || current_->pixels.empty()) {
        if (!emptyReason_.empty()) {
            ui::drawText(dc, emptyReason_, client, ui::theme().secondaryText,
                         ui::font(window_, ui::FontRole::Body),
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        buffer_.blit(target, width, height);
        return;
    }

    blitPixels(dc, current_->pixels.data(), current_->width, current_->height, client);

    auto toRect = [&](const RectD& r) {
        RectD v = viewRect(r);
        RECT out;
        out.left = LONG(std::lround(v.minX()));
        out.top = LONG(std::lround(v.minY()));
        out.right = LONG(std::lround(v.maxX()));
        out.bottom = LONG(std::lround(v.maxY()));
        return out;
    };

    if (hoveredCell_ >= 0 && hoveredCell_ < int(current_->cells.size())) {
        RECT outline = toRect(current_->cells[size_t(hoveredCell_)].rect);
        InflateRect(&outline, -1, -1);
        if (outline.right > outline.left && outline.bottom > outline.top) {
            ui::frame(dc, outline, RGB(255, 255, 255), 1);
        }
    }

    if (highlighted_ >= 0 && current_) {
        auto it = current_->nodeRects.find(highlighted_);
        if (it != current_->nodeRects.end()) {
            RECT outline = toRect(it->second);
            // A small block would otherwise have its outline drawn on top of the
            // few pixels that identify it, so the frame goes outside instead.
            bool small = (outline.right - outline.left) < 26 || (outline.bottom - outline.top) < 26;
            if (small) InflateRect(&outline, 2, 2);
            ui::frame(dc, outline, RGB(255, 255, 255), 2);
            RECT inner = outline;
            InflateRect(&inner, -2, -2);
            if (inner.right > inner.left && inner.bottom > inner.top) {
                ui::frame(dc, inner, RGB(20, 20, 20), 1);
            }
        }
    }

    buffer_.blit(target, width, height);
}

// MARK: - Interaction

void TreemapView::updateHover(int x, int y) {
    if (!current_ || current_->width == 0) return;
    RECT client;
    GetClientRect(window_, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    int px = int(double(x) * double(current_->width) / double(width));
    int py = int(double(y) * double(current_->height) / double(height));
    int32_t cell = current_->cellIndexAt(px, py);
    if (cell == hoveredCell_) return;
    hoveredCell_ = cell;
    InvalidateRect(window_, nullptr, FALSE);
    showTooltip(x, y);
}

void TreemapView::showTooltip(int, int) {
    if (!tooltip_ || !tooltipContent) return;
    std::wstring title, detail;
    bool have = false;
    if (hoveredCell_ >= 0 && hoveredCell_ < int(current_->cells.size())) {
        const TreemapCell& cell = current_->cells[size_t(hoveredCell_)];
        have = tooltipContent(cell.node, cell.aggregatedCount, &title, &detail);
    }
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.hwnd = window_;
    info.uId = UINT_PTR(window_);
    if (!have) {
        SendMessageW(tooltip_, TTM_ACTIVATE, FALSE, 0);
        return;
    }
    std::wstring text = detail.empty() ? title : title + L"\n" + detail;
    info.lpszText = text.data();
    SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, LPARAM(&info));
    SendMessageW(tooltip_, TTM_ACTIVATE, TRUE, 0);
}

void TreemapView::hideTooltip() {
    if (tooltip_) SendMessageW(tooltip_, TTM_ACTIVATE, FALSE, 0);
    if (hoveredCell_ != -1) {
        hoveredCell_ = -1;
        InvalidateRect(window_, nullptr, FALSE);
    }
}

LRESULT TreemapView::handle(UINT message, WPARAM w, LPARAM l) {
    switch (message) {
        case WM_CREATE: {
            tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                       WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                                       window_, nullptr, GetModuleHandleW(nullptr), nullptr);
            TOOLINFOW info{};
            info.cbSize = sizeof(info);
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            info.hwnd = window_;
            info.uId = UINT_PTR(window_);
            info.lpszText = const_cast<wchar_t*>(L"");
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, LPARAM(&info));
            SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 480);
            return 0;
        }
        case kRenderDone: {
            auto* payload = reinterpret_cast<std::pair<TreemapImage*, double>*>(l);
            if (int(w) == renderGeneration_.load()) {
                onRenderComplete(payload->first, payload->second);
            } else {
                delete payload->first;
                // A stale result still means the render slot is free.
                rendering_ = false;
                if (renderQueued_) requestRender();
            }
            delete payload;
            return 0;
        }
        case WM_TIMER:
            if (w == kAnimationTimer) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_SIZE:
            requestRender();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window_, &ps);
            paint(dc);
            EndPaint(window_, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window_, 0};
            TrackMouseEvent(&track);
            updateHover(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        }
        case WM_MOUSELEAVE:
            hideTooltip();
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(window_);
            updateHover(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            int32_t node = nodeForCell(hoveredCell_);
            if (node >= 0 && onSelect) onSelect(node);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int32_t node = nodeForCell(hoveredCell_);
            if (node >= 0 && arena_) {
                // Double-clicking a file zooms into the folder holding it, which
                // is the only thing that can actually be drawn as a map.
                if (!arena_->isDirectory(size_t(uint32_t(node)))) {
                    node = arena_->parent[size_t(uint32_t(node))];
                }
                zoomTo(node);
            }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            updateHover(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            int32_t node = nodeForCell(hoveredCell_);
            if (node >= 0 && onSelect) onSelect(node);
            // Forwarded so the parent can put up the same menu the list uses.
            POINT screen{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(window_, &screen);
            SendMessageW(GetParent(window_), WM_CONTEXTMENU, WPARAM(window_),
                         MAKELPARAM(screen.x, screen.y));
            return 0;
        }
        case WM_MBUTTONDOWN:
            zoomOut();
            return 0;
        case WM_DESTROY:
            // Any render in flight posts to a window that no longer exists, and
            // PostMessage failing is what frees the image in that case.
            ++renderGeneration_;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window_, message, w, l);
}

}  // namespace app
