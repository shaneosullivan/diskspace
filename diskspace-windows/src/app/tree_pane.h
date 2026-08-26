#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "treemap.h"
#include "ui_common.h"

namespace app {

/// The file tree, size ordered, with the Size column pinned to the right edge.
///
/// Drawn rather than assembled from a common control. A standard tree view cannot
/// hold a column still while the names beside it scroll sideways, and deep
/// indentation eventually pushes the figure the user came for out of view. Here
/// the names scroll under a column that never moves.
///
/// Rows are materialised only for what is expanded, so the cost of the pane is
/// proportional to what is on screen rather than to the size of the tree.
class TreePane {
public:
    static TreePane* create(HWND parent, int id);
    HWND window() const { return window_; }

    void setArena(const NodeArena* arena, const std::wstring& rootLabel);
    void setPalette(KindPalette* palette) { palette_ = palette; }
    void setMetric(SizeMetric metric);
    void setGeneration(int generation);
    /// Column heading for the size column, which names whichever figure is on
    /// show so a screenshot of the window is never ambiguous.
    void setSizeColumnTitle(const std::wstring& title);
    /// Supplies the figure to display, honouring the physical/logical setting.
    std::function<int64_t(size_t node)> displaySize;

    void refresh();
    /// Collapses everything back to the root row, which is then reopened on the
    /// next refresh so the user lands on the top-level breakdown.
    void collapseToRoot();
    void resetExpansion();
    /// Expands the path to `node`, selects it and scrolls it into view.
    void reveal(int32_t node);
    int32_t selectedNode() const { return selected_; }

    std::function<void(int32_t)> onSelect;
    /// Fired for the two buttons that live in the header row.
    std::function<void()> onCollapseAll;
    std::function<void()> onToggleSidePanel;
    /// True when the file-kind pane is on screen, which decides the button glyph.
    bool sidePanelVisible = true;

private:
    struct Row {
        int32_t node;
        int depth;
        bool expandable;
    };

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT handle(UINT message, WPARAM w, LPARAM l);

    void rebuildRows();
    void appendRows(int32_t node, int depth);
    const std::vector<int32_t>& sortedChildren(int32_t node);
    void paint(HDC dc);
    void paintHeader(HDC dc, const RECT& bounds);
    void paintRow(HDC dc, const RECT& bounds, const Row& row, bool selected, bool alternate);
    void updateScrollBars();
    int rowHeight() const;
    int headerHeight() const;
    int sizeColumnWidth() const;
    int rowAt(int y) const;
    void select(int rowIndex, bool scrollIntoView);
    void scrollToRow(int rowIndex, bool centre);
    void toggleExpansion(int32_t node);
    std::wstring nameOf(size_t node) const;
    /// Widest name currently on screen, for the horizontal scroll range.
    int contentWidth(HDC dc);

    HWND window_ = nullptr;
    const NodeArena* arena_ = nullptr;
    KindPalette* palette_ = nullptr;
    std::wstring rootLabel_;
    std::wstring sizeTitle_ = L"Size";
    SizeMetric metric_ = SizeMetric::Total;

    std::vector<Row> rows_;
    std::unordered_set<int32_t> expanded_;
    /// Children sorted by size, cached per node and invalidated by generation so
    /// a live scan does not resort the whole tree on every refresh.
    std::unordered_map<int32_t, std::vector<int32_t>> sortedCache_;
    int cacheGeneration_ = -1;
    SizeMetric cacheMetric_ = SizeMetric::Total;
    int generation_ = 0;
    bool didExpandRoot_ = false;

    int scrollY_ = 0;
    int scrollX_ = 0;
    int contentWidth_ = 0;
    int32_t selected_ = -1;
    int hoveredHeaderButton_ = -1;
    ui::BackBuffer buffer_;
};

}  // namespace app
