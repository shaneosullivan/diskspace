#pragma once

#include <functional>
#include <vector>

#include "tree_scanner.h"
#include "treemap.h"
#include "widgets.h"
#include "x11.h"

namespace app {

/// The file-kind table: every kind found, what it occupies, how many files it
/// covers. Sorted by clicking a heading.
///
/// The figures come straight from the scanner, which maintains them as entries
/// are applied, so redrawing costs O(kinds) rather than a sweep of every node.
class KindPane {
public:
    void setBounds(const ui::Rect& bounds) { bounds_ = bounds; }
    const ui::Rect& bounds() const { return bounds_; }

    void setScanner(const diskscan::TreeScanner* scanner) { scanner_ = scanner; }
    void setPalette(const KindPalette* palette) { palette_ = palette; }
    void setMetric(diskscan::SizeMetric metric) { metric_ = metric; }
    void setUseLogicalSize(bool logical) { useLogicalSize_ = logical; }
    void refresh();

    /// Kinds currently shown, biggest first under the default sort. The window
    /// hands this to the palette, which gives its most distinct hues to whatever
    /// fills the most space.
    const std::vector<diskscan::TreeScanner::KindTotal>& rows() const { return rows_; }

    /// The height this would like, so a short table does not leave a gap.
    int preferredHeight(const Context& context) const;

    void draw(ui::Canvas& canvas, const Context& context);

    bool mousePress(int x, int y, int button, const Context& context);
    bool mouseDrag(int x, int y);
    void mouseRelease() { scrollbar_.release(); }
    bool scroll(int x, int y, int steps, const Context& context);

    /// A kind was chosen, so the map can dim everything else.
    std::function<void(int16_t)> onSelected;
    std::function<void()> onNeedsRedraw;

private:
    enum class SortKey { Kind, Size, Files };

    void applySort();
    /// Left edge of each column at this width. The kind name takes what is left.
    void columns(const Context& context, int* swatch, int* name, int* size, int* files) const;
    int headerHeight(const Context& context) const;
    void redraw() {
        if (onNeedsRedraw) onNeedsRedraw();
    }

    ui::Rect bounds_;
    Scrollbar scrollbar_;

    const diskscan::TreeScanner* scanner_ = nullptr;
    const KindPalette* palette_ = nullptr;
    diskscan::SizeMetric metric_ = diskscan::SizeMetric::Total;
    bool useLogicalSize_ = false;

    std::vector<diskscan::TreeScanner::KindTotal> rows_;
    SortKey sortKey_ = SortKey::Size;
    bool ascending_ = false;
    int16_t selected_ = -1;
};

}  // namespace app
