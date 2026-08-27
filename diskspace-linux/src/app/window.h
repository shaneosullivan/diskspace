#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

#include "platform.h"
#include "scan_controller.h"
#include "tree_pane.h"
#include "treemap_area.h"
#include "treemap.h"

namespace app {

/// The window: a chooser until something is scanned, then the results.
class MainWindow {
public:
    explicit MainWindow(GtkApplication* application);

    GtkWidget* widget() { return window_; }

    /// Begin scanning `path` immediately, as a command line argument asks.
    void scanPath(const diskscan::NameString& path);

private:
    GtkWidget* buildWelcome();
    GtkWidget* buildResults();
    void showWelcome();

    void onScanProgress();
    void onScanFinished();
    void updateStatus();
    void updateLegend();
    void refreshViews();
    void setMetric(diskscan::SizeMetric metric);
    void chooseFolder();
    void rescan();

    static void legendDraw(GtkDrawingArea* area, cairo_t* cr, int width, int height, void* self);

    GtkApplication* application_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* stack_ = nullptr;
    GtkWidget* header_ = nullptr;
    GtkWidget* status_ = nullptr;
    GtkWidget* hoverLabel_ = nullptr;
    GtkWidget* legend_ = nullptr;
    GtkWidget* metricDropdown_ = nullptr;
    GtkWidget* rescanButton_ = nullptr;
    GtkWidget* chooseButton_ = nullptr;
    GtkWidget* stopButton_ = nullptr;

    std::unique_ptr<TreePane> tree_;
    std::unique_ptr<TreemapArea> map_;
    ScanController scan_;
    KindPalette palette_;
    TreemapOptions options_;

    std::vector<diskscan::TreeScanner::KindTotal> kinds_;
    /// The scan that is showing, which is not always the one that is running.
    diskscan::NameString shownRoot_;
    bool haveResults_ = false;
};

}  // namespace app
