#include "window.h"

#include <algorithm>

#include "file_kind.h"
#include "format.h"
#include "preferences.h"
#include "preferences_window.h"
#include "product.h"
#include "theme.h"
#include "update_check.h"

namespace app {
namespace {

using diskscan::NameString;
using diskscan::SizeMetric;

const char* const kMetricNames[] = {
    "Current size",
    "Added in the last month",
    "Added in the last six months",
    "Added since last scan",
    nullptr,
};

/// Ids for the two blocks drawn alongside the scan. Negative, so they cannot
/// collide with a node index.
constexpr int32_t kFreeSpaceId = -2;
constexpr int32_t kOtherSpaceId = -3;

GtkWidget* boldLabel(const char* text) {
    GtkWidget* label = gtk_label_new(text);
    PangoAttrList* attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(label), attributes);
    pango_attr_list_unref(attributes);
    return label;
}

/// A row for one volume on the chooser: where it is, what it holds, how full.
GtkWidget* volumeCard(const diskscan::VolumeInfo& volume) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    GtkWidget* title = boldLabel(volume.mountPoint.c_str());
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), title);

    const std::string detail = humanBytes(volume.usedBytes) + " used of " +
                               humanBytes(volume.totalBytes) + "  ·  " + volume.fileSystem;
    GtkWidget* subtitle = gtk_label_new(detail.c_str());
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_widget_add_css_class(subtitle, "dim-label");
    gtk_box_append(GTK_BOX(box), subtitle);

    GtkWidget* bar = gtk_progress_bar_new();
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(bar),
        volume.totalBytes > 0 ? double(volume.usedBytes) / double(volume.totalBytes) : 0.0);
    gtk_box_append(GTK_BOX(box), bar);
    return box;
}

}  // namespace

MainWindow::MainWindow(GtkApplication* application) : application_(application) {
    diskscan::FileKind::warmUp();

    window_ = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(window_), DISKSPACE_APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(window_), 1180, 760);

    header_ = buildHeader();
    gtk_window_set_titlebar(GTK_WINDOW(window_), header_);

    stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(GTK_STACK(stack_), buildWelcome(), "welcome");
    gtk_stack_add_named(GTK_STACK(stack_), buildResults(), "results");

    banner_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_visible(banner_, FALSE);

    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(stack_, TRUE);
    gtk_box_append(GTK_BOX(column), banner_);
    gtk_box_append(GTK_BOX(column), stack_);
    gtk_window_set_child(GTK_WINDOW(window_), column);

    scan_.onProgress = [this] { onScanProgress(); };
    scan_.onFinished = [this] { onScanFinished(); };
    Preferences::shared().addObserver([this] { onPreferencesChanged(); });

    applyOptions();
    applyLayout();
    showWelcome();
    checkForUpdates();
}

// MARK: - Chrome

GtkWidget* MainWindow::buildHeader() {
    GtkWidget* header = gtk_header_bar_new();

    GtkWidget* open = gtk_button_new_with_label("Open…");
    gtk_widget_set_tooltip_text(open, "Choose a folder to scan (Ctrl+O)");
    g_signal_connect_swapped(open, "clicked", G_CALLBACK(+[](gpointer self) {
                                 static_cast<MainWindow*>(self)->chooseFolder();
                             }),
                             this);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open);

    rescanButton_ = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(rescanButton_, "Scan again (Ctrl+R)");
    g_signal_connect_swapped(rescanButton_, "clicked", G_CALLBACK(+[](gpointer self) {
                                 static_cast<MainWindow*>(self)->rescan();
                             }),
                             this);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), rescanButton_);

    zoomOutButton_ = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(zoomOutButton_, "Back out of this folder");
    g_signal_connect_swapped(zoomOutButton_, "clicked", G_CALLBACK(+[](gpointer self) {
                                 static_cast<MainWindow*>(self)->zoomOut();
                             }),
                             this);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), zoomOutButton_);

    stopButton_ = gtk_button_new_with_label("Stop");
    g_signal_connect_swapped(stopButton_, "clicked", G_CALLBACK(+[](gpointer self) {
                                 static_cast<MainWindow*>(self)->scan_.stop();
                             }),
                             this);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), stopButton_);

    // The overflow menu, for everything that is not worth a button of its own.
    GMenu* menu = g_menu_new();
    GMenu* fileSection = g_menu_new();
    g_menu_append(fileSection, "Rescan This Folder", "win.rescan-folder");
    g_menu_append(fileSection, "Copy Path", "win.copy-path");
    g_menu_append(fileSection, "Open", "win.open-selected");
    g_menu_append(fileSection, "Show in Files", "win.reveal");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(fileSection));
    GMenu* dangerSection = g_menu_new();
    g_menu_append(dangerSection, "Move to Trash", "win.trash");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(dangerSection));
    GMenu* appSection = g_menu_new();
    g_menu_append(appSection, "Settings…", "win.preferences");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(appSection));

    GtkWidget* button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), button);
    g_object_unref(menu);

    metricDropdown_ = gtk_drop_down_new_from_strings(kMetricNames);
    gtk_widget_set_tooltip_text(metricDropdown_,
                                "Which measure the list and the map are ranked by");
    g_signal_connect_swapped(
        metricDropdown_, "notify::selected",
        G_CALLBACK(+[](gpointer self, GParamSpec*, GtkDropDown* dropdown) {
            static_cast<MainWindow*>(self)->setMetric(
                SizeMetric(gtk_drop_down_get_selected(dropdown)));
        }),
        this);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), metricDropdown_);

    // The actions the menu and the keyboard both go through, so there is one
    // implementation of each and the shortcuts cannot drift from the menu.
    struct Action {
        const char* name;
        void (MainWindow::*handler)();
        const char* accelerator;
    };
    // Named rather than written inline: G_CALLBACK is a macro, and the comma in
    // std::pair<A, B> reads as a second macro argument long before the compiler
    // sees a template.
    using Binding = std::pair<MainWindow*, const Action*>;
    static const Action kActions[] = {
        {"rescan-folder", &MainWindow::rescanFolder, nullptr},
        {"copy-path", &MainWindow::copyPath, "<Control>c"},
        {"open-selected", &MainWindow::openSelected, nullptr},
        {"reveal", &MainWindow::revealSelected, nullptr},
        {"trash", &MainWindow::moveToTrash, "Delete"},
        {"preferences", nullptr, "<Control>comma"},
        {"open", &MainWindow::chooseFolder, "<Control>o"},
        {"rescan", &MainWindow::rescan, "<Control>r"},
        {"zoom-out", &MainWindow::zoomOut, "<Control>Up"},
    };
    for (const Action& action : kActions) {
        GSimpleAction* item = g_simple_action_new(action.name, nullptr);
        auto* holder = new Binding(this, &action);
        g_signal_connect_data(
            item, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
                auto* pair = static_cast<Binding*>(data);
                if (pair->second->handler) {
                    (pair->first->*(pair->second->handler))();
                } else {
                    presentPreferences(GTK_WINDOW(pair->first->window_));
                }
            }),
            holder,
            [](gpointer data, GClosure*) { delete static_cast<Binding*>(data); },
            GConnectFlags(0));
        g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(item));
        g_object_unref(item);

        if (action.accelerator) {
            const std::string full = std::string("win.") + action.name;
            const char* accelerators[] = {action.accelerator, nullptr};
            gtk_application_set_accels_for_action(application_, full.c_str(), accelerators);
        }
    }
    return header;
}

GtkWidget* MainWindow::buildWelcome() {
    GtkWidget* scroller = gtk_scrolled_window_new();
    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(column, 36);
    gtk_widget_set_margin_bottom(column, 36);
    gtk_widget_set_margin_start(column, 36);
    gtk_widget_set_margin_end(column, 36);
    gtk_widget_set_halign(column, GTK_ALIGN_CENTER);

    GtkWidget* heading = gtk_label_new(nullptr);
    gtk_label_set_markup(
        GTK_LABEL(heading),
        "<span size='xx-large' weight='bold'>Find your missing disk space</span>");
    gtk_box_append(GTK_BOX(column), heading);

    GtkWidget* sub = gtk_label_new("Choose a drive or a folder to scan.");
    gtk_widget_add_css_class(sub, "dim-label");
    gtk_box_append(GTK_BOX(column), sub);

    // Two columns, as the macOS and Windows builds use, so a machine with several
    // drives does not push the button off the bottom of the window.
    GtkWidget* grid = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid), 2);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid), 1);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(grid), 12);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(grid), 12);
    gtk_widget_set_margin_top(grid, 12);
    // Wide enough for two cards side by side, with room for the padding the flow
    // box puts around each. Exactly twice the card width is not enough: the cards
    // fit, and then the padding does not.
    gtk_widget_set_size_request(grid, 700, -1);

    for (const diskscan::VolumeInfo& volume : diskscan::mountedVolumes()) {
        GtkWidget* button = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(button), volumeCard(volume));
        gtk_widget_set_size_request(button, 320, -1);

        // The path is owned by the button, so it outlives this loop.
        char* path = g_strdup(diskscan::volumeScanPath(volume).c_str());
        g_object_set_data_full(G_OBJECT(button), "scan-path", path, g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* self, gpointer data) {
                             const char* target = static_cast<const char*>(
                                 g_object_get_data(G_OBJECT(self), "scan-path"));
                             static_cast<MainWindow*>(data)->scanPath(target);
                         }),
                         this);
        gtk_flow_box_append(GTK_FLOW_BOX(grid), button);
    }
    gtk_box_append(GTK_BOX(column), grid);

    GtkWidget* folder = gtk_button_new_with_label("Choose a Folder…");
    gtk_widget_set_halign(folder, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(folder, 8);
    g_signal_connect_swapped(folder, "clicked", G_CALLBACK(+[](gpointer self) {
                                 static_cast<MainWindow*>(self)->chooseFolder();
                             }),
                             this);
    gtk_box_append(GTK_BOX(column), folder);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), column);
    return scroller;
}

GtkWidget* MainWindow::buildResults() {
    tree_ = std::make_unique<TreePane>();
    map_ = std::make_unique<TreemapArea>();
    kinds_ = std::make_unique<KindPane>();
    tree_->setPalette(&palette_);
    map_->setPalette(&palette_);
    kinds_->setPalette(&palette_);

    // Selecting in either pane selects in the other, so the list and the map are
    // two views of one thing rather than two separate tools.
    tree_->onSelected = [this](int32_t node) { map_->setSelected(node); };
    tree_->onActivated = [this](int32_t node) { zoomTo(node); };
    map_->onSelected = [this](int32_t node) { tree_->revealNode(node); };
    map_->onActivated = [this](int32_t node) { zoomTo(node); };
    map_->onContextMenu = [this](int32_t node, double x, double y) {
        if (node >= 0) {
            menuTarget_ = node;
            tree_->revealNode(node);
            map_->setSelected(node);
        }
        showContextMenu(x, y);
    };
    map_->onHovered = [this](int32_t node) {
        if (node < 0 || !scan_.scanner()) {
            gtk_label_set_text(GTK_LABEL(hoverLabel_), "");
            return;
        }
        const NodeArena& arena = scan_.scanner()->arena();
        if (size_t(uint32_t(node)) >= arena.count()) return;
        const NameString path = arena.path(size_t(uint32_t(node)));
        const std::string text = displayName(path.c_str(), path.size()) + "   " +
                                 humanBytes(arena.size(options_.metric, size_t(uint32_t(node))));
        gtk_label_set_text(GTK_LABEL(hoverLabel_), text.c_str());
    };

    // The left column: the file list, with the kind table under it.
    leftPaned_ = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_start_child(GTK_PANED(leftPaned_), tree_->widget());
    gtk_paned_set_end_child(GTK_PANED(leftPaned_), kinds_->widget());
    gtk_paned_set_position(GTK_PANED(leftPaned_), 430);
    gtk_paned_set_resize_start_child(GTK_PANED(leftPaned_), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(leftPaned_), FALSE);

    outerPaned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(outerPaned_), leftPaned_);
    gtk_paned_set_end_child(GTK_PANED(outerPaned_), map_->widget());
    gtk_paned_set_position(GTK_PANED(outerPaned_), 470);
    gtk_paned_set_resize_start_child(GTK_PANED(outerPaned_), TRUE);
    gtk_paned_set_resize_end_child(GTK_PANED(outerPaned_), TRUE);

    breadcrumbs_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_start(breadcrumbs_, 10);
    gtk_widget_set_margin_end(breadcrumbs_, 10);
    gtk_widget_set_margin_top(breadcrumbs_, 6);

    status_ = gtk_label_new("");
    gtk_widget_set_halign(status_, GTK_ALIGN_START);
    gtk_widget_add_css_class(status_, "dim-label");

    hoverLabel_ = gtk_label_new("");
    gtk_widget_set_halign(hoverLabel_, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(hoverLabel_), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(hoverLabel_, "dim-label");

    GtkWidget* footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(footer, 10);
    gtk_widget_set_margin_end(footer, 10);
    gtk_widget_set_margin_bottom(footer, 8);
    gtk_box_append(GTK_BOX(footer), status_);
    gtk_box_append(GTK_BOX(footer), hoverLabel_);

    contextMenu_ = gtk_popover_menu_new_from_model(nullptr);
    gtk_widget_set_parent(contextMenu_, window_);
    gtk_popover_set_has_arrow(GTK_POPOVER(contextMenu_), FALSE);

    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(outerPaned_, TRUE);
    gtk_box_append(GTK_BOX(column), breadcrumbs_);
    gtk_box_append(GTK_BOX(column), outerPaned_);
    gtk_box_append(GTK_BOX(column), footer);
    return column;
}

// MARK: - Scanning

void MainWindow::showWelcome() {
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "welcome");
    gtk_widget_set_visible(rescanButton_, FALSE);
    gtk_widget_set_visible(stopButton_, FALSE);
    gtk_widget_set_visible(metricDropdown_, FALSE);
    gtk_widget_set_visible(zoomOutButton_, FALSE);
    gtk_window_set_title(GTK_WINDOW(window_), DISKSPACE_APP_NAME);
}

void MainWindow::scanPath(const NameString& path) {
    shownRoot_ = path;
    mapRoot_ = 0;
    menuTarget_ = -1;
    haveVolume_ = diskscan::volumeInfoForPath(path, &volume_);

    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "results");
    gtk_widget_set_visible(stopButton_, TRUE);
    gtk_widget_set_visible(metricDropdown_, TRUE);
    gtk_widget_set_visible(rescanButton_, FALSE);
    gtk_window_set_title(GTK_WINDOW(window_), displayName(path.c_str(), path.size()).c_str());

    scan_.start(path);

    // Pointed at the new scan's arena, which is a different object from the last
    // one - the panes would otherwise be reading freed memory.
    const NodeArena& arena = scan_.scanner()->arena();
    tree_->setArena(&arena);
    tree_->setRoot(0);
    map_->setArena(&arena);
    map_->setRoot(0);
    map_->setSelected(-1);
    kinds_->setScanner(scan_.scanner());
    applyOptions();
    updateBreadcrumbs();
}

void MainWindow::onScanProgress() {
    refreshViews();
    updateStatus();
}

void MainWindow::onScanFinished() {
    gtk_widget_set_visible(stopButton_, FALSE);
    gtk_widget_set_visible(rescanButton_, TRUE);
    // The volume's free space has moved if anything was deleted during the scan,
    // and the blocks drawn beside the map are sized from it.
    if (!shownRoot_.empty()) haveVolume_ = diskscan::volumeInfoForPath(shownRoot_, &volume_);
    applyOptions();
    refreshViews();
    updateStatus();
    updateBreadcrumbs();
}

void MainWindow::refreshViews() {
    if (!scan_.scanner()) return;

    kinds_->refresh();
    // The palette hands its most distinct hues to whatever fills the most space,
    // so it has to be told the ranking before either pane draws with it. The kind
    // table's rows are already in that order under its default sort; asked for
    // separately here so a user sorting the table by name cannot recolour the map.
    std::vector<diskscan::TreeScanner::KindTotal> ranked =
        scan_.scanner()->kindTotals(options_.metric, options_.useLogicalSize);
    std::sort(ranked.begin(), ranked.end(),
              [](const diskscan::TreeScanner::KindTotal& a,
                 const diskscan::TreeScanner::KindTotal& b) { return a.bytes > b.bytes; });
    std::vector<int16_t> ranking;
    ranking.reserve(ranked.size());
    for (const auto& kind : ranked) ranking.push_back(kind.kind);
    palette_.setRanking(ranking);

    tree_->refresh();
    map_->invalidate();
}

void MainWindow::updateStatus() {
    if (!scan_.scanner()) return;
    const NodeArena& arena = scan_.scanner()->arena();
    const size_t count = arena.count();
    if (count == 0) {
        gtk_label_set_text(GTK_LABEL(status_), "Scanning…");
        return;
    }

    const int64_t total =
        options_.useLogicalSize ? arena.logical[0] : arena.size(options_.metric, 0);
    std::string text = humanBytes(total) + " in " + grouped(int64_t(count)) + " items";
    if (scan_.running()) {
        text += "  ·  scanning…";
    } else {
        const diskscan::ScanResult& result = scan_.result();
        char elapsed[64];
        std::snprintf(elapsed, sizeof(elapsed), "  ·  %.2fs", result.elapsed);
        text += elapsed;
        if (result.hardlinkDuplicates > 0) {
            text += "  ·  " + grouped(result.hardlinkDuplicates) + " hard links counted once (" +
                    humanBytes(result.hardlinkDuplicateBytes) + ")";
        }
        if (result.errors > 0) text += "  ·  " + grouped(result.errors) + " unreadable";
        if (result.stopped) text += "  ·  stopped";
    }
    gtk_label_set_text(GTK_LABEL(status_), text.c_str());
}

// MARK: - Zooming

void MainWindow::zoomTo(int32_t node) {
    if (!scan_.scanner() || node <= 0) return;
    const NodeArena& arena = scan_.scanner()->arena();
    if (size_t(uint32_t(node)) >= arena.count()) return;
    if (!arena.isDirectory(size_t(uint32_t(node)))) return;

    mapRoot_ = node;
    map_->setRoot(node);
    tree_->revealNode(node);
    updateBreadcrumbs();
}

void MainWindow::zoomOut() {
    if (!scan_.scanner() || mapRoot_ == 0) return;
    const NodeArena& arena = scan_.scanner()->arena();
    const int32_t parent = arena.parent[size_t(uint32_t(mapRoot_))];
    // zoomTo declines node 0, which is what it should do for a double click on
    // the background - but backing all the way out is exactly how the root is
    // reached, so it is said here instead.
    if (parent == 0) {
        mapRoot_ = 0;
        map_->setRoot(0);
        tree_->revealNode(0);
        updateBreadcrumbs();
        return;
    }
    zoomTo(parent);
}

void MainWindow::updateBreadcrumbs() {
    while (GtkWidget* child = gtk_widget_get_first_child(breadcrumbs_)) {
        gtk_box_remove(GTK_BOX(breadcrumbs_), child);
    }
    gtk_widget_set_visible(zoomOutButton_, mapRoot_ != 0);
    if (!scan_.scanner()) return;
    const NodeArena& arena = scan_.scanner()->arena();
    if (arena.count() == 0) return;

    // Root first, which means walking up and then reversing.
    std::vector<int32_t> chain;
    size_t current = size_t(uint32_t(mapRoot_));
    for (;;) {
        chain.push_back(int32_t(current));
        if (current == 0) break;
        current = size_t(uint32_t(arena.parent[current]));
    }
    std::reverse(chain.begin(), chain.end());

    // Named for the same reason as Binding above: the comma would otherwise be
    // read as a macro argument.
    using Crumb = std::pair<MainWindow*, int32_t>;

    for (size_t i = 0; i < chain.size(); ++i) {
        if (i > 0) {
            GtkWidget* separator = gtk_label_new("›");
            gtk_widget_add_css_class(separator, "dim-label");
            gtk_box_append(GTK_BOX(breadcrumbs_), separator);
        }
        const size_t node = size_t(uint32_t(chain[i]));
        const std::string name =
            node == 0 ? displayName(shownRoot_.c_str(), shownRoot_.size())
                      : displayName(arena.nameData(node), arena.nameSize(node));

        GtkWidget* crumb = gtk_button_new_with_label(name.c_str());
        gtk_widget_add_css_class(crumb, "flat");
        gtk_widget_set_sensitive(crumb, i + 1 < chain.size());
        auto* holder = new Crumb(this, chain[i]);
        g_signal_connect_data(
            crumb, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                auto* pair = static_cast<Crumb*>(data);
                if (pair->second == 0) {
                    pair->first->mapRoot_ = 0;
                    pair->first->map_->setRoot(0);
                    pair->first->updateBreadcrumbs();
                } else {
                    pair->first->zoomTo(pair->second);
                }
            }),
            holder,
            [](gpointer data, GClosure*) { delete static_cast<Crumb*>(data); },
            GConnectFlags(0));
        gtk_box_append(GTK_BOX(breadcrumbs_), crumb);
    }
}

// MARK: - Settings and options

void MainWindow::applyLayout() {
    const bool horizontal = Preferences::shared().splitHorizontally();

    // The inner split runs across the outer one. With the map beside the list,
    // the kind table sits under the list; with the map below, it sits beside it.
    // Nesting two splits the same way would stack three panes in a column and
    // leave the kind table one row tall, which is what it did at first.
    gtk_orientable_set_orientation(GTK_ORIENTABLE(outerPaned_),
                                   horizontal ? GTK_ORIENTATION_VERTICAL
                                              : GTK_ORIENTATION_HORIZONTAL);
    gtk_orientable_set_orientation(GTK_ORIENTABLE(leftPaned_),
                                   horizontal ? GTK_ORIENTATION_HORIZONTAL
                                              : GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_position(GTK_PANED(outerPaned_), horizontal ? 340 : 470);
    gtk_paned_set_position(GTK_PANED(leftPaned_), horizontal ? 640 : 430);
    gtk_widget_set_visible(kinds_->widget(), Preferences::shared().showFileKinds());
}

void MainWindow::applyOptions() {
    const Preferences& preferences = Preferences::shared();
    options_.useLogicalSize = !preferences.showPhysicalFileSize();
    options_.extraBlocks = extraBlocks();
    palette_.consistentColours = preferences.consistentKindColours();

    if (tree_) tree_->setUseLogicalSize(options_.useLogicalSize);
    if (kinds_) kinds_->setUseLogicalSize(options_.useLogicalSize);
    if (map_) map_->setOptions(options_);
}

void MainWindow::onPreferencesChanged() {
    applyLayout();
    applyOptions();
    refreshViews();
    updateStatus();
}

std::vector<ExtraBlock> MainWindow::extraBlocks() const {
    std::vector<ExtraBlock> blocks;
    if (!haveVolume_ || volume_.totalBytes <= 0 || !scan_.scanner()) return blocks;
    const NodeArena& arena = scan_.scanner()->arena();
    if (arena.count() == 0) return blocks;

    const Preferences& preferences = Preferences::shared();
    if (preferences.showFreeSpace()) {
        ExtraBlock block;
        block.id = kFreeSpaceId;
        block.label = "Free space";
        block.bytes = volume_.freeBytes;
        block.colour = 0x2B3648;
        blocks.push_back(block);
    }
    if (preferences.showOtherSpace()) {
        // What is in use on the volume but outside what was scanned. Nothing, and
        // so not drawn, when a whole volume is on screen.
        const int64_t elsewhere = volume_.usedBytes - arena.allocated[0];
        if (elsewhere > 0) {
            ExtraBlock block;
            block.id = kOtherSpaceId;
            block.label = "Other space in use";
            block.bytes = elsewhere;
            block.colour = 0x4A4458;
            blocks.push_back(block);
        }
    }
    return blocks;
}

void MainWindow::setMetric(SizeMetric metric) {
    options_.metric = metric;
    tree_->setMetric(metric);
    kinds_->setMetric(metric);
    map_->setOptions(options_);
    refreshViews();
    updateStatus();
}

// MARK: - Actions

int32_t MainWindow::actionTarget() const {
    if (menuTarget_ >= 0) return menuTarget_;
    const int32_t selected = tree_->selected();
    return selected >= 0 ? selected : mapRoot_;
}

NameString MainWindow::pathOf(int32_t node) const {
    if (!scan_.scanner() || node < 0) return NameString();
    const NodeArena& arena = scan_.scanner()->arena();
    if (size_t(uint32_t(node)) >= arena.count()) return NameString();
    return arena.path(size_t(uint32_t(node)));
}

void MainWindow::copyPath() {
    const NameString path = pathOf(actionTarget());
    if (path.empty()) return;
    // Through the display form: a clipboard is text, and a name that is not valid
    // UTF-8 cannot be put on one as it stands.
    const std::string text = displayName(path.c_str(), path.size());
    gdk_clipboard_set_text(gtk_widget_get_clipboard(window_), text.c_str());
}

void MainWindow::openSelected() {
    const NameString path = pathOf(actionTarget());
    if (path.empty()) return;
    GFile* file = g_file_new_for_path(path.c_str());
    GtkFileLauncher* launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, GTK_WINDOW(window_), nullptr, nullptr, nullptr);
    g_object_unref(launcher);
    g_object_unref(file);
}

void MainWindow::revealSelected() {
    const NameString path = pathOf(actionTarget());
    if (path.empty()) return;
    GFile* file = g_file_new_for_path(path.c_str());
    GtkFileLauncher* launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_open_containing_folder(launcher, GTK_WINDOW(window_), nullptr, nullptr,
                                             nullptr);
    g_object_unref(launcher);
    g_object_unref(file);
}

void MainWindow::moveToTrash() {
    const int32_t node = actionTarget();
    const NameString path = pathOf(node);
    if (path.empty() || node <= 0) return;

    GtkAlertDialog* dialog = gtk_alert_dialog_new(
        "Move to trash?");
    const std::string detail = displayName(path.c_str(), path.size());
    gtk_alert_dialog_set_detail(dialog, detail.c_str());
    const char* buttons[] = {"Cancel", "Move to Trash", nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);

    using Pending = std::pair<MainWindow*, int32_t>;
    auto* holder = new Pending(this, node);
    gtk_alert_dialog_choose(
        dialog, GTK_WINDOW(window_), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<std::pair<MainWindow*, int32_t>> pair(
                static_cast<std::pair<MainWindow*, int32_t>*>(data));  // not in a macro here
            GError* error = nullptr;
            const int chosen =
                gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
            if (error) {
                g_error_free(error);
                return;
            }
            if (chosen != 1) return;

            MainWindow* self = pair->first;
            const NameString path = self->pathOf(pair->second);
            if (path.empty()) return;
            GFile* file = g_file_new_for_path(path.c_str());
            GError* trashError = nullptr;
            if (g_file_trash(file, nullptr, &trashError)) {
                // Taken out of every figure rather than rescanned: the entry stays
                // in the tree at zero, which is what the other builds do.
                self->scan_.scanner()->remove(pair->second);
                self->refreshViews();
                self->updateStatus();
            } else if (trashError) {
                GtkAlertDialog* failure = gtk_alert_dialog_new("Could not move it to the trash.");
                gtk_alert_dialog_set_detail(failure, trashError->message);
                gtk_alert_dialog_show(failure, GTK_WINDOW(self->window_));
                g_object_unref(failure);
                g_error_free(trashError);
            }
            g_object_unref(file);
        },
        holder);
    g_object_unref(dialog);
}

void MainWindow::showContextMenu(double x, double y) {
    GMenu* menu = g_menu_new();
    g_menu_append(menu, "Copy Path", "win.copy-path");
    g_menu_append(menu, "Open", "win.open-selected");
    g_menu_append(menu, "Show in Files", "win.reveal");
    GMenu* danger = g_menu_new();
    g_menu_append(danger, "Move to Trash", "win.trash");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(danger));

    gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(contextMenu_), G_MENU_MODEL(menu));
    g_object_unref(menu);

    // Positioned against the window, so the coordinates have to be translated out
    // of the map's own space first.
    graphene_point_t from = {float(x), float(y)};
    graphene_point_t to = from;
    // False when the two are not in the same window, which cannot happen here.
    // If it somehow did, the untranslated point stands: the menu opens somewhere
    // odd rather than not at all.
    if (!gtk_widget_compute_point(map_->widget(), window_, &from, &to)) to = from;
    const GdkRectangle at = {int(to.x), int(to.y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(contextMenu_), &at);
    gtk_popover_popup(GTK_POPOVER(contextMenu_));
}

void MainWindow::rescan() {
    if (!shownRoot_.empty()) scanPath(shownRoot_);
}

void MainWindow::rescanFolder() {
    const int32_t node = actionTarget();
    if (!scan_.scanner() || node <= 0 || scan_.running()) return;
    const NodeArena& arena = scan_.scanner()->arena();
    if (!arena.isDirectory(size_t(uint32_t(node)))) return;
    scan_.scanner()->rescan(node);
    refreshViews();
    updateStatus();
}

void MainWindow::chooseFolder() {
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose a folder to scan");
    gtk_file_dialog_select_folder(
        dialog, GTK_WINDOW(window_), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            GError* error = nullptr;
            GFile* file =
                gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
            if (error) {
                // Dismissed, which is not a failure worth reporting.
                g_error_free(error);
                return;
            }
            if (!file) return;
            char* path = g_file_get_path(file);
            if (path) {
                static_cast<MainWindow*>(data)->scanPath(path);
                g_free(path);
            }
            g_object_unref(file);
        },
        this);
    g_object_unref(dialog);
}

// MARK: - Updates

void MainWindow::checkForUpdates() {
    UpdateCheck::shared().check([this](const UpdateAvailable& update) {
        showUpdateBanner(update.summary(), update.downloads);
    });
}

void MainWindow::showUpdateBanner(const std::string& text, const std::string& url) {
    while (GtkWidget* child = gtk_widget_get_first_child(banner_)) {
        gtk_box_remove(GTK_BOX(banner_), child);
    }
    gtk_widget_add_css_class(banner_, "toolbar");
    gtk_widget_set_margin_top(banner_, 6);
    gtk_widget_set_margin_bottom(banner_, 6);
    gtk_widget_set_margin_start(banner_, 10);
    gtk_widget_set_margin_end(banner_, 10);

    GtkWidget* label = gtk_label_new(text.c_str());
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(banner_), label);

    if (!url.empty()) {
        GtkWidget* link = gtk_link_button_new_with_label(url.c_str(), "Download");
        gtk_box_append(GTK_BOX(banner_), link);
    }

    GtkWidget* dismiss = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(dismiss, "flat");
    g_signal_connect_swapped(dismiss, "clicked", G_CALLBACK(+[](gpointer banner) {
                                 gtk_widget_set_visible(GTK_WIDGET(banner), FALSE);
                             }),
                             banner_);
    gtk_box_append(GTK_BOX(banner_), dismiss);
    gtk_widget_set_visible(banner_, TRUE);
}

}  // namespace app
