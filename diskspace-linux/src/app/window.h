#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

#include "kind_pane.h"
#include "platform.h"
#include "scan_controller.h"
#include "tree_pane.h"
#include "treemap.h"
#include "treemap_area.h"

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
    GtkWidget* buildHeader();
    void showWelcome();

    void onScanProgress();
    void onScanFinished();
    void onPreferencesChanged();
    void updateStatus();
    void updateBreadcrumbs();
    void refreshViews();
    void applyLayout();
    void applyOptions();

    void setMetric(diskscan::SizeMetric metric);
    void zoomTo(int32_t node);
    void zoomOut();
    void chooseFolder();
    void rescan();
    void rescanFolder();
    void checkForUpdates();
    void showUpdateBanner(const std::string& text, const std::string& url);

    /// The node every action applies to: whatever is selected, or the root.
    int32_t actionTarget() const;
    diskscan::NameString pathOf(int32_t node) const;
    void copyPath();
    void openSelected();
    void revealSelected();
    void moveToTrash();
    void showContextMenu(double x, double y);

    /// Free space, and everything else in use on the volume, as blocks alongside
    /// what was scanned.
    std::vector<ExtraBlock> extraBlocks() const;

    GtkApplication* application_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* stack_ = nullptr;
    GtkWidget* header_ = nullptr;
    GtkWidget* status_ = nullptr;
    GtkWidget* hoverLabel_ = nullptr;
    GtkWidget* breadcrumbs_ = nullptr;
    GtkWidget* banner_ = nullptr;
    GtkWidget* metricDropdown_ = nullptr;
    GtkWidget* rescanButton_ = nullptr;
    GtkWidget* stopButton_ = nullptr;
    GtkWidget* zoomOutButton_ = nullptr;
    GtkWidget* outerPaned_ = nullptr;
    GtkWidget* leftPaned_ = nullptr;
    GtkWidget* contextMenu_ = nullptr;

    std::unique_ptr<TreePane> tree_;
    std::unique_ptr<TreemapArea> map_;
    std::unique_ptr<KindPane> kinds_;
    ScanController scan_;
    KindPalette palette_;
    TreemapOptions options_;

    diskscan::VolumeInfo volume_;
    bool haveVolume_ = false;
    /// The scan that is showing, which is not always the one that is running.
    diskscan::NameString shownRoot_;
    /// Where the map is zoomed to, which is the root until something is opened.
    int32_t mapRoot_ = 0;
    /// Where the last context menu was opened, for the actions it offers.
    int32_t menuTarget_ = -1;
};

}  // namespace app
