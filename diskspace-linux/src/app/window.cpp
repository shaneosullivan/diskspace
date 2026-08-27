#include "window.h"

#include <algorithm>

#include "file_kind.h"
#include "format.h"
#include "theme.h"
#include "product.h"

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

GtkWidget* boldLabel(const char* text) {
    GtkWidget* label = gtk_label_new(text);
    PangoAttrList* attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(label), attributes);
    pango_attr_list_unref(attributes);
    return label;
}

/// A row for one volume on the chooser: where it is, what it holds, and how full.
GtkWidget* volumeCard(const diskscan::VolumeInfo& volume) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    GtkWidget* title = boldLabel(volume.mountPoint.c_str());
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), title);

    const int64_t used = volume.totalBytes - volume.freeBytes;
    const std::string detail = humanBytes(used) + " used of " + humanBytes(volume.totalBytes) +
                               "  ·  " + volume.fileSystem;
    GtkWidget* subtitle = gtk_label_new(detail.c_str());
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_widget_add_css_class(subtitle, "dim-label");
    gtk_box_append(GTK_BOX(box), subtitle);

    GtkWidget* bar = gtk_progress_bar_new();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar),
                                  volume.totalBytes > 0
                                      ? double(used) / double(volume.totalBytes)
                                      : 0.0);
    gtk_box_append(GTK_BOX(box), bar);
    return box;
}

}  // namespace

MainWindow::MainWindow(GtkApplication* application) : application_(application) {
    diskscan::FileKind::warmUp();

    window_ = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(window_), DISKSPACE_APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(window_), 1180, 720);

    header_ = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(window_), header_);

    chooseButton_ = gtk_button_new_with_label("Open…");
    g_signal_connect_swapped(chooseButton_, "clicked", G_CALLBACK(+[](gpointer self) {
                                 static_cast<MainWindow*>(self)->chooseFolder();
                             }),
                             this);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header_), chooseButton_);

    rescanButton_ = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(rescanButton_, "Scan again");
    g_signal_connect_swapped(rescanButton_, "clicked", G_CALLBACK(+[](gpointer self) {
                                 static_cast<MainWindow*>(self)->rescan();
                             }),
                             this);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header_), rescanButton_);

    stopButton_ = gtk_button_new_with_label("Stop");
    g_signal_connect_swapped(stopButton_, "clicked", G_CALLBACK(+[](gpointer self) {
                                 static_cast<MainWindow*>(self)->scan_.stop();
                             }),
                             this);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header_), stopButton_);

    metricDropdown_ = gtk_drop_down_new_from_strings(kMetricNames);
    gtk_widget_set_tooltip_text(metricDropdown_,
                                "Which measure the list and the map are ranked by");
    g_signal_connect_swapped(
        metricDropdown_, "notify::selected", G_CALLBACK(+[](gpointer self, GParamSpec*,
                                                            GtkDropDown* dropdown) {
            static_cast<MainWindow*>(self)->setMetric(
                SizeMetric(gtk_drop_down_get_selected(dropdown)));
        }),
        this);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header_), metricDropdown_);

    stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(GTK_STACK(stack_), buildWelcome(), "welcome");
    gtk_stack_add_named(GTK_STACK(stack_), buildResults(), "results");
    gtk_window_set_child(GTK_WINDOW(window_), stack_);

    scan_.onProgress = [this] { onScanProgress(); };
    scan_.onFinished = [this] { onScanFinished(); };

    showWelcome();
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

    // Two columns, as the macOS and Windows builds use, so a machine with
    // several drives does not push the button off the bottom of the window.
    GtkWidget* grid = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid), 2);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid), 1);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(grid), 12);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(grid), 12);
    gtk_widget_set_margin_top(grid, 12);
    // Wide enough for two cards side by side, with room for the padding the flow
    // box puts around each. Without it the centred column hands the flow box only
    // its natural width - one card - and the two-column setting above never gets
    // to apply. Exactly twice the card width is not enough: it fits, and then the
    // padding does not.
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
    tree_->setPalette(&palette_);
    map_->setPalette(&palette_);

    // Selecting in either pane selects in the other, so the list and the map are
    // two views of one thing rather than two separate tools.
    tree_->onSelected = [this](int32_t node) { map_->setSelected(node); };
    map_->onSelected = [this](int32_t node) { tree_->revealNode(node); };
    map_->onActivated = [this](int32_t node) {
        if (!scan_.scanner()) return;
        const NodeArena& arena = scan_.scanner()->arena();
        if (node <= 0 || !arena.isDirectory(size_t(uint32_t(node)))) return;
        map_->setRoot(node);
        tree_->revealNode(node);
        updateStatus();
    };
    map_->onHovered = [this](int32_t node) {
        if (node < 0 || !scan_.scanner()) {
            gtk_label_set_text(GTK_LABEL(hoverLabel_), "");
            return;
        }
        const NodeArena& arena = scan_.scanner()->arena();
        if (size_t(uint32_t(node)) >= arena.count()) return;
        const std::string path = displayName(arena.path(size_t(uint32_t(node))).c_str(),
                                             arena.path(size_t(uint32_t(node))).size());
        const std::string text =
            path + "   " + humanBytes(arena.size(options_.metric, size_t(uint32_t(node))));
        gtk_label_set_text(GTK_LABEL(hoverLabel_), text.c_str());
    };

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 460);
    gtk_paned_set_start_child(GTK_PANED(paned), tree_->widget());
    gtk_paned_set_end_child(GTK_PANED(paned), map_->widget());
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);

    legend_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(legend_, -1, 26);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(legend_),
                                   GtkDrawingAreaDrawFunc(&MainWindow::legendDraw), this,
                                   nullptr);

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
    gtk_box_append(GTK_BOX(footer), legend_);
    gtk_box_append(GTK_BOX(footer), status_);
    gtk_box_append(GTK_BOX(footer), hoverLabel_);

    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(column), paned);
    gtk_box_append(GTK_BOX(column), footer);
    return column;
}

void MainWindow::showWelcome() {
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "welcome");
    gtk_widget_set_visible(rescanButton_, FALSE);
    gtk_widget_set_visible(stopButton_, FALSE);
    gtk_widget_set_visible(metricDropdown_, FALSE);
    gtk_window_set_title(GTK_WINDOW(window_), DISKSPACE_APP_NAME);
}

void MainWindow::scanPath(const NameString& path) {
    shownRoot_ = path;
    haveResults_ = false;

    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "results");
    gtk_widget_set_visible(stopButton_, TRUE);
    gtk_widget_set_visible(metricDropdown_, TRUE);
    gtk_widget_set_visible(rescanButton_, FALSE);
    gtk_window_set_title(GTK_WINDOW(window_), displayName(path.c_str(), path.size()).c_str());

    scan_.start(path);

    // Pointed at the new scan's arena, which is a different object from the last
    // one - the old panes would otherwise be reading freed memory.
    const NodeArena& arena = scan_.scanner()->arena();
    tree_->setArena(&arena);
    tree_->setRoot(0);
    map_->setArena(&arena);
    map_->setRoot(0);
    map_->setSelected(-1);
}

void MainWindow::onScanProgress() {
    refreshViews();
    updateStatus();
}

void MainWindow::onScanFinished() {
    haveResults_ = true;
    gtk_widget_set_visible(stopButton_, FALSE);
    gtk_widget_set_visible(rescanButton_, TRUE);
    refreshViews();
    updateStatus();
}

void MainWindow::refreshViews() {
    if (!scan_.scanner()) return;

    // Ordered biggest first, which kindTotals does not do - it returns kinds in
    // id order and leaves ranking to whoever asked. Both things below need the
    // order: the legend reads top down, and the palette hands its most distinct
    // hues to the kinds that fill the most space.
    kinds_ = scan_.scanner()->kindTotals(options_.metric, options_.useLogicalSize);
    std::sort(kinds_.begin(), kinds_.end(),
              [](const diskscan::TreeScanner::KindTotal& a,
                 const diskscan::TreeScanner::KindTotal& b) { return a.bytes > b.bytes; });

    std::vector<int16_t> ranking;
    ranking.reserve(kinds_.size());
    for (const auto& kind : kinds_) ranking.push_back(kind.kind);
    palette_.setRanking(ranking);

    tree_->refresh();
    map_->invalidate();
    updateLegend();
}

void MainWindow::updateStatus() {
    if (!scan_.scanner()) return;
    const NodeArena& arena = scan_.scanner()->arena();
    const size_t count = arena.count();
    if (count == 0) {
        gtk_label_set_text(GTK_LABEL(status_), "Scanning…");
        return;
    }

    std::string text = humanBytes(arena.allocated[0]) + " in " + grouped(int64_t(count)) +
                       " items";
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

void MainWindow::updateLegend() { gtk_widget_queue_draw(legend_); }

void MainWindow::legendDraw(GtkDrawingArea*, cairo_t* cr, int width, int height, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (self->kinds_.empty()) return;

    const GdkRGBA text = foreground(self->legend_);

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(layout, font);

    const std::vector<diskscan::NameString>& names = diskscan::FileKind::names();
    double x = 0;
    const double y = (height - 12) / 2.0;
    for (const auto& kind : self->kinds_) {
        if (kind.bytes <= 0) continue;
        if (x > width - 120) break;

        const uint32_t colour = self->palette_.colour(kind.kind);
        cairo_set_source_rgb(cr, ((colour >> 16) & 0xFF) / 255.0, ((colour >> 8) & 0xFF) / 255.0,
                             (colour & 0xFF) / 255.0);
        cairo_rectangle(cr, x, y, 10, 10);
        cairo_fill(cr);

        const size_t index = size_t(uint32_t(kind.kind));
        const std::string label = (index < names.size() ? names[index] : std::string("Other")) +
                                  "  " + humanBytes(kind.bytes);
        gdk_cairo_set_source_rgba(cr, &text);
        pango_layout_set_text(layout, label.c_str(), -1);
        cairo_move_to(cr, x + 14, y - 2);
        pango_cairo_show_layout(cr, layout);

        int textWidth = 0;
        pango_layout_get_pixel_size(layout, &textWidth, nullptr);
        x += 14 + textWidth + 18;
    }

    pango_font_description_free(font);
    g_object_unref(layout);
}

void MainWindow::setMetric(SizeMetric metric) {
    options_.metric = metric;
    tree_->setMetric(metric);
    map_->setOptions(options_);
    refreshViews();
}

void MainWindow::rescan() {
    if (!shownRoot_.empty()) scanPath(shownRoot_);
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

}  // namespace app
