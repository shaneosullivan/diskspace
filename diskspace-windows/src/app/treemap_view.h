#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "treemap.h"
#include "ui_common.h"

namespace app {

/// The treemap pane.
///
/// Layout and rasterisation happen on a background thread; this window only ever
/// blits a finished buffer and draws the outlines on top. Hit testing reads the
/// id buffer that came with the pixels, so hover stays instant however many
/// rectangles are on screen.
class TreemapView {
public:
    static TreemapView* create(HWND parent, int id);

    HWND window() const { return window_; }

    void setArena(const NodeArena* arena);
    void setMetric(SizeMetric metric);
    void setOptions(const TreemapOptions& options);
    void setPalette(KindPalette* palette) { palette_ = palette; }

    /// Redraw now: settings changed, the metric changed, or a scan finished.
    void refresh();
    /// The periodic tick during a scan, which the view paces itself against.
    void liveRefresh();
    void resetLivePacing();

    void setHighlightedNode(int32_t node);
    int32_t highlightedNode() const { return highlighted_; }

    bool canZoomOut() const { return zoomStack_.size() > 1; }
    int32_t zoomedNode() const { return zoomStack_.empty() ? 0 : zoomStack_.back().node; }
    const std::vector<int32_t> zoomChain() const;
    void zoomTo(int32_t node);
    void zoomOut();
    void resetZoom();

    /// Called when a block is clicked, with the node it stands for.
    std::function<void(int32_t)> onSelect;
    /// Called when the view zooms, so the window can update its buttons.
    std::function<void()> onZoomChanged;
    /// Supplies the two lines of the hover tooltip. Return false for no tooltip.
    std::function<bool(int32_t node, int32_t aggregated, std::wstring* title,
                       std::wstring* detail)>
        tooltipContent;

    const TreemapImage* currentImage() const { return current_.get(); }
    /// The node a cell stands for, resolving pooled cells to their folder.
    int32_t nodeForCell(int cellIndex) const;

private:
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT handle(UINT message, WPARAM w, LPARAM l);
    void paint(HDC dc);
    void requestRender();
    void onRenderComplete(TreemapImage* image, double milliseconds);
    void recordRenderCost(double seconds);
    void updateHover(int x, int y);
    void showTooltip(int x, int y);
    void hideTooltip();
    RectD viewRect(const RectD& rect) const;
    void beginTransition(const RectD& focus, bool zoomingIn);
    void endTransition();

    HWND window_ = nullptr;
    HWND tooltip_ = nullptr;
    const NodeArena* arena_ = nullptr;
    KindPalette* palette_ = nullptr;
    std::unique_ptr<TreemapImage> current_;
    TreemapOptions options_;
    ui::BackBuffer buffer_;

    int hoveredCell_ = -1;
    int32_t highlighted_ = -1;
    std::wstring emptyReason_;

    /// Bumped for every render request, so a result that arrives after the view
    /// has moved on is discarded rather than drawn.
    std::atomic<int> renderGeneration_{0};
    bool rendering_ = false;
    bool renderQueued_ = false;

    // Live refresh pacing. A full render of a large tree costs tens of
    // milliseconds; running one every tick would spend the whole scan drawing.
    static constexpr double kFastInterval = 1.0 / 4.0;
    static constexpr double kSlowInterval = 1.0;
    /// Above this share of the interval, the render is judged too expensive to
    /// keep repeating and the interval is stretched.
    static constexpr double kOverBudget = 0.85;
    static constexpr double kComfortable = 0.65;
    ULONGLONG lastLiveRefresh_ = 0;
    double averageRenderCost_ = 0;
    double liveInterval_ = kFastInterval;

    struct ZoomEntry {
        int32_t node;
        RectD rectInParent;
    };
    std::vector<ZoomEntry> zoomStack_{{0, RectD{}}};

    struct Transition {
        std::vector<uint32_t> outgoing;
        int outgoingWidth = 0;
        int outgoingHeight = 0;
        RectD focus;
        bool zoomingIn = true;
        ULONGLONG start = 0;
    };
    std::unique_ptr<Transition> transition_;
};

}  // namespace app
