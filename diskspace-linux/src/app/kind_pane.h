#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <vector>

#include "tree_scanner.h"
#include "treemap.h"

namespace app {

/// The file-kind table: every kind found, what it occupies, how many files it
/// covers. Sorted by clicking a heading.
///
/// The figures come straight from the scanner, which maintains them as entries
/// are applied, so redrawing costs O(kinds) rather than a sweep of every node.
class KindPane {
public:
    KindPane();

    GtkWidget* widget() { return scroller_; }

    void setScanner(const diskscan::TreeScanner* scanner) { scanner_ = scanner; }
    void setPalette(const KindPalette* palette) { palette_ = palette; }
    void setMetric(diskscan::SizeMetric metric) { metric_ = metric; }
    void setUseLogicalSize(bool logical) { useLogicalSize_ = logical; }
    void refresh();

    /// Kinds currently shown, biggest first under the default sort. The window
    /// hands this to the palette, which gives its most distinct hues to whatever
    /// fills the most space.
    const std::vector<diskscan::TreeScanner::KindTotal>& rows() const { return rows_; }

    /// A kind was chosen, so the map can dim everything else.
    std::function<void(int16_t)> onSelected;

private:
    enum class SortKey { Kind, Size, Files };

    static void drawTrampoline(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                               void* self);
    static void pressTrampoline(GtkGestureClick* gesture, int count, double x, double y,
                                void* self);

    void draw(cairo_t* cr, int width, int height);
    void applySort();
    /// Left edge of each column at this width. The kind name takes what is left.
    void columns(int width, int* swatch, int* name, int* size, int* files) const;

    GtkWidget* scroller_ = nullptr;
    GtkWidget* area_ = nullptr;
    GtkWidget* header_ = nullptr;
    GtkWidget* column_ = nullptr;

    const diskscan::TreeScanner* scanner_ = nullptr;
    const KindPalette* palette_ = nullptr;
    diskscan::SizeMetric metric_ = diskscan::SizeMetric::Total;
    bool useLogicalSize_ = false;

    std::vector<diskscan::TreeScanner::KindTotal> rows_;
    SortKey sortKey_ = SortKey::Size;
    bool ascending_ = false;
    int16_t selected_ = -1;

    static constexpr int kRowHeight = 20;
    static constexpr int kHeaderHeight = 22;
    static constexpr int kSizeColumn = 90;
    static constexpr int kFilesColumn = 70;
};

}  // namespace app
