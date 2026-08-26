#pragma once

#include <windows.h>

#include <functional>
#include <vector>

#include "../core/tree_scanner.h"
#include "treemap.h"
#include "ui_common.h"

namespace app {

/// The file-kind column: every kind found, with what it occupies and how many
/// files it covers. Sorted by clicking a heading.
///
/// The figures come straight from the scanner, which maintains them as entries
/// are applied, so redrawing this pane costs O(kinds) rather than a sweep of
/// every node.
class KindPane {
public:
    static KindPane* create(HWND parent, int id);
    HWND window() const { return window_; }

    void setScanner(diskscan::TreeScanner* scanner) { scanner_ = scanner; }
    void setPalette(KindPalette* palette) { palette_ = palette; }
    void setMetric(SizeMetric metric) { metric_ = metric; }
    void setUseLogicalSize(bool logical) { useLogicalSize_ = logical; }
    void refresh();

    /// Width the pane needs to show every column in full, so reopening it lands
    /// at a size where nothing is clipped.
    int preferredWidth() const;

private:
    enum class SortKey { Kind, Size, Files };

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT handle(UINT message, WPARAM w, LPARAM l);
    void paint(HDC dc);
    void applySort();
    void updateScrollBar();
    int rowHeight() const;
    int headerHeight() const;
    /// Left edge and width of each column, in device pixels.
    void columns(int width, RECT* colour, RECT* kind, RECT* size, RECT* files) const;

    HWND window_ = nullptr;
    diskscan::TreeScanner* scanner_ = nullptr;
    KindPalette* palette_ = nullptr;
    SizeMetric metric_ = SizeMetric::Total;
    bool useLogicalSize_ = false;
    std::vector<diskscan::TreeScanner::KindTotal> rows_;
    SortKey sortKey_ = SortKey::Size;
    bool ascending_ = false;
    int scrollY_ = 0;
    ui::BackBuffer buffer_;
};

}  // namespace app
