#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

#include "node_arena.h"
#include "treemap.h"

namespace app {

using diskscan::NodeArena;
using diskscan::SizeMetric;

/// The file list: every folder ranked by whichever measure is selected, with the
/// biggest first and folders expandable in place.
///
/// Drawn rather than assembled out of widgets. A scan reaches millions of nodes,
/// and a list model that materialised a widget per row - or even per visible row,
/// rebuilt ten times a second while the scan runs - would spend more time in the
/// toolkit than in the scan. Only the rows on screen are ever touched.
class TreePane {
public:
    TreePane();

    GtkWidget* widget() { return scroller_; }

    void setArena(const NodeArena* arena);
    void setPalette(const KindPalette* palette);
    void setRoot(int32_t node);
    void setMetric(SizeMetric metric);
    void setUseLogicalSize(bool logical);

    /// Rebuild the visible rows from the arena. Called on every scan tick.
    void refresh();

    void selectNode(int32_t node);
    int32_t selected() const { return selected_; }

    /// Expand from the root down to `node` so it can be shown, then select it.
    void revealNode(int32_t node);

    std::function<void(int32_t)> onSelected;
    std::function<void(int32_t)> onActivated;

private:
    struct Row {
        int32_t node = 0;
        int depth = 0;
        bool hasChildren = false;
        bool expanded = false;
    };

    static void drawTrampoline(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                               void* self);
    static void pressTrampoline(GtkGestureClick* gesture, int count, double x, double y,
                                void* self);

    void draw(cairo_t* cr, int width, int height);
    void appendRows(int32_t node, int depth);
    void rebuild();
    int64_t sizeOf(int32_t node) const;
    int rowAt(double y) const;
    void scrollToRow(int index);

    GtkWidget* scroller_ = nullptr;
    GtkWidget* area_ = nullptr;
    const NodeArena* arena_ = nullptr;
    const KindPalette* palette_ = nullptr;

    std::vector<Row> rows_;
    std::unordered_set<int32_t> expanded_;
    int32_t root_ = 0;
    int32_t selected_ = -1;
    SizeMetric metric_ = SizeMetric::Total;
    bool useLogicalSize_ = false;

    /// Scratch, reused so that rebuilding allocates nothing per directory.
    std::vector<std::pair<int64_t, int32_t>> scratch_;

    static constexpr int kRowHeight = 22;
    static constexpr int kIndent = 16;
    static constexpr int kSizeColumn = 110;
};

}  // namespace app
