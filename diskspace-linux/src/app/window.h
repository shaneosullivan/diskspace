#pragma once

#include <memory>
#include <string>
#include <vector>

#include "folder_chooser.h"
#include "kind_pane.h"
#include "platform.h"
#include "preferences_window.h"
#include "scan_controller.h"
#include "tree_pane.h"
#include "treemap.h"
#include "treemap_area.h"
#include "widgets.h"
#include "x11.h"

namespace app {

/// The window: a chooser until something is scanned, then the results.
///
/// One X window, and everything in it drawn. Menus, dialogs and the settings are
/// overlays rather than top-levels, which is what makes that practical: there is
/// one place that lays out, one place that paints and one place that decides who
/// gets a click.
class MainWindow {
public:
    explicit MainWindow(ui::Display* display);
    ~MainWindow();

    bool create();
    void run();

    /// Begin scanning `path` immediately, as a command line argument asks.
    void scanPath(const diskscan::NameString& path);

    /// Draw one frame to a PPM file and exit, once the scan has settled.
    ///
    /// How this build gets looked at without a person at a screen, and the only
    /// way that works under a compositor that will not hand over a window's
    /// pixels. Used by the screenshot script and worth keeping for a UI change
    /// that needs checking against the last one.
    void captureAndExit(const std::string& path, double afterSeconds);

    ui::Window& window() { return window_; }

private:
    /// A yes-or-no question, drawn over everything else.
    struct Confirm {
        bool open = false;
        std::string title;
        std::string detail;
        std::string confirmLabel;
        /// True for something irreversible, which colours the button.
        bool destructive = false;
        std::function<void()> onConfirm;
        ui::Rect panel;
        Button confirmButton;
        Button cancelButton;
    };

    // Painting.
    void draw(ui::Canvas& canvas);
    void drawHeader(ui::Canvas& canvas, const ui::Rect& bounds);
    void drawWelcome(ui::Canvas& canvas, const ui::Rect& bounds);
    void drawStatus(ui::Canvas& canvas, const ui::Rect& bounds);
    void drawBanner(ui::Canvas& canvas, const ui::Rect& bounds);
    void drawConfirm(ui::Canvas& canvas);
    void layoutResults(const ui::Rect& bounds);

    // Events.
    void onMousePress(const ui::MouseEvent& event);
    void onMouseRelease(const ui::MouseEvent& event);
    void onMouseMove(const ui::MouseEvent& event);
    void onScroll(const ui::MouseEvent& event, int dx, int dy);
    void onKey(const ui::KeyEvent& event);
    void tick();

    // Scanning.
    void showWelcome();
    void onScanProgress();
    void onScanFinished();
    void onPreferencesChanged();
    void refreshViews();
    void applyOptions();
    void setMetric(diskscan::SizeMetric metric);

    void zoomTo(int32_t node);
    void zoomOut();
    void chooseFolder();
    void rescan();
    void rescanFolder();
    void checkForUpdates();

    /// The node every action applies to: whatever is selected, or the root.
    int32_t actionTarget() const;
    diskscan::NameString pathOf(int32_t node) const;
    void copyPath();
    void openSelected();
    void revealSelected();
    void moveToTrash();
    void showContextMenu(int32_t node, int x, int y);
    void showApplicationMenu(int x, int y);
    void showMetricMenu(int x, int y);
    void ask(const std::string& title, const std::string& detail, const std::string& confirmLabel,
             bool destructive, std::function<void()> action);

    /// Free space, and everything else in use on the volume, as blocks alongside
    /// what was scanned.
    std::vector<ExtraBlock> extraBlocks() const;
    std::string statusText() const;
    void redraw() { window_.invalidate(); }

    ui::Display* display_ = nullptr;
    ui::Window window_;
    Context context_;

    TreePane tree_;
    TreemapArea map_;
    KindPane kinds_;
    FolderChooser chooser_;
    PreferencesPanel settings_;
    Menu menu_;
    Confirm confirm_;

    ScanController scan_;
    KindPalette palette_;
    TreemapOptions options_;

    // The header's controls, laid out on every paint and hit-tested in place.
    Button openButton_;
    Button rescanButton_;
    Button stopButton_;
    Button zoomOutButton_;
    Button metricButton_;
    Button menuButton_;
    std::vector<std::pair<ui::Rect, int32_t>> crumbs_;

    Button chooseFolderButton_;
    std::vector<std::pair<ui::Rect, diskscan::NameString>> volumeCards_;
    std::vector<diskscan::VolumeInfo> volumes_;

    diskscan::VolumeInfo volume_;
    bool haveVolume_ = false;
    bool showingResults_ = false;

    /// The scan that is showing, which is not always the one that is running.
    diskscan::NameString shownRoot_;
    /// Where the map is zoomed to, which is the root until something is opened.
    int32_t mapRoot_ = 0;
    /// Where the last context menu was opened, for the actions it offers.
    int32_t menuTarget_ = -1;
    /// What the pointer is over on the map, shown beside the status.
    std::string hoverText_;

    std::string capturePath_;
    double captureAfter_ = 0;
    int captureTicks_ = 0;

    // The update banner.
    bool bannerVisible_ = false;
    std::string bannerText_;
    std::string bannerUrl_;
    Button bannerButton_;
    Button bannerDismiss_;

    /// Splitter positions, in pixels from the top or left of the content area.
    int outerSplit_ = 0;
    int innerSplit_ = 0;
    /// Which splitter is being dragged: 0 none, 1 outer, 2 inner.
    int draggingSplit_ = 0;
    ui::Rect outerSplitBounds_;
    ui::Rect innerSplitBounds_;
};

}  // namespace app
