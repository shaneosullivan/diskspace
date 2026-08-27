#include "tree_pane.h"

#include <algorithm>

#include "file_kind.h"
#include "format.h"
#include "theme.h"

namespace app {

TreePane::TreePane() {
    area_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_),
                                   GtkDrawingAreaDrawFunc(&TreePane::drawTrampoline), this,
                                   nullptr);

    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(&TreePane::pressTrampoline), this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(click));

    scroller_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller_), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), area_);
    gtk_widget_set_hexpand(scroller_, TRUE);
    gtk_widget_set_vexpand(scroller_, TRUE);
}

void TreePane::setArena(const NodeArena* arena) {
    arena_ = arena;
    expanded_.clear();
    selected_ = -1;
    rebuild();
}

void TreePane::setPalette(const KindPalette* palette) { palette_ = palette; }

void TreePane::setRoot(int32_t node) {
    root_ = node;
    // The root itself is always open, or the pane would show one closed row.
    expanded_.insert(node);
    rebuild();
}

void TreePane::setMetric(SizeMetric metric) {
    metric_ = metric;
    rebuild();
}

void TreePane::setUseLogicalSize(bool logical) {
    useLogicalSize_ = logical;
    rebuild();
}

void TreePane::refresh() { rebuild(); }

int64_t TreePane::sizeOf(int32_t node) const {
    if (!arena_) return 0;
    const size_t i = size_t(uint32_t(node));
    if (useLogicalSize_ && metric_ == SizeMetric::Total) return arena_->logical[i];
    return arena_->size(metric_, i);
}

void TreePane::appendRows(int32_t node, int depth) {
    if (!arena_) return;
    const size_t index = size_t(uint32_t(node));
    const auto children = arena_->children(index);

    const bool isDir = arena_->isDirectory(index);
    const bool hasChildren = isDir && !children.empty();
    const bool isExpanded = hasChildren && expanded_.count(node) != 0;

    rows_.push_back(Row{node, depth, hasChildren, isExpanded});
    if (!isExpanded) return;

    // Ranked by the measure on display, so switching to "added last month"
    // reorders the whole list rather than just recolouring it.
    const size_t first = rows_.size();
    (void)first;
    std::vector<std::pair<int64_t, int32_t>> ordered;
    ordered.reserve(children.size());
    for (size_t child = children.begin; child < children.end; ++child) {
        ordered.emplace_back(sizeOf(int32_t(child)), int32_t(child));
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;  // stable enough to stop rows jittering
    });

    for (const auto& entry : ordered) {
        // Nothing with no bytes at all, unless it is a folder that might still
        // fill up - a scan in progress would otherwise flicker rows in and out.
        if (entry.first == 0 && !arena_->isDirectory(size_t(uint32_t(entry.second)))) continue;
        appendRows(entry.second, depth + 1);
    }
}

void TreePane::rebuild() {
    rows_.clear();
    if (arena_ && arena_->count() > 0) appendRows(root_, 0);
    gtk_widget_set_size_request(area_, -1, int(rows_.size()) * kRowHeight);
    gtk_widget_queue_draw(area_);
}

void TreePane::drawTrampoline(GtkDrawingArea*, cairo_t* cr, int width, int height, void* self) {
    static_cast<TreePane*>(self)->draw(cr, width, height);
}

void TreePane::draw(cairo_t* cr, int width, int height) {
    if (!arena_) return;

    const GdkRGBA text = foreground(area_);
    const GdkRGBA accent = selection(area_);

    // Only the rows the viewport actually covers. GTK gives the drawing area a
    // clip for the exposed region, so this asks it what that is rather than
    // walking every row of a tree that may hold millions.
    double clipX1, clipY1, clipX2, clipY2;
    cairo_clip_extents(cr, &clipX1, &clipY1, &clipX2, &clipY2);
    const int firstRow = std::max(0, int(clipY1) / kRowHeight);
    const int lastRow = std::min(int(rows_.size()), int(clipY2) / kRowHeight + 2);
    (void)height;

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(layout, font);

    for (int i = firstRow; i < lastRow; ++i) {
        const Row& row = rows_[size_t(i)];
        const size_t node = size_t(uint32_t(row.node));
        const double y = double(i) * kRowHeight;

        if (row.node == selected_) {
            gdk_cairo_set_source_rgba(cr, &accent);
            cairo_rectangle(cr, 0, y, width, kRowHeight);
            cairo_fill(cr);
        }

        const double indent = 6 + row.depth * kIndent;

        // The expander, drawn as a triangle rather than themed: one shape, and it
        // lines up with the row grid at any font size.
        if (row.hasChildren) {
            cairo_set_source_rgba(cr, text.red, text.green, text.blue, 0.75);
            const double cx = indent + 4, cy = y + kRowHeight / 2.0;
            if (row.expanded) {
                cairo_move_to(cr, cx - 4, cy - 2);
                cairo_line_to(cr, cx + 4, cy - 2);
                cairo_line_to(cr, cx, cy + 3);
            } else {
                cairo_move_to(cr, cx - 2, cy - 4);
                cairo_line_to(cr, cx + 3, cy);
                cairo_line_to(cr, cx - 2, cy + 4);
            }
            cairo_close_path(cr);
            cairo_fill(cr);
        }

        // The kind's colour, the same one the treemap uses for it, so the two
        // panes read as one system.
        if (palette_) {
            const uint32_t colour = palette_->colour(arena_->kind[node]);
            cairo_set_source_rgb(cr, ((colour >> 16) & 0xFF) / 255.0,
                                 ((colour >> 8) & 0xFF) / 255.0, (colour & 0xFF) / 255.0);
            cairo_rectangle(cr, indent + 12, y + kRowHeight / 2.0 - 4, 8, 8);
            cairo_fill(cr);
        }

        const std::string name =
            displayName(arena_->nameData(node), arena_->nameSize(node));
        if (row.node == selected_) {
            cairo_set_source_rgb(cr, 1, 1, 1);
        } else {
            gdk_cairo_set_source_rgba(cr, &text);
        }

        const int nameLeft = int(indent) + 26;
        const int nameWidth = std::max(20, width - nameLeft - kSizeColumn - 8);
        pango_layout_set_text(layout, name.c_str(), -1);
        pango_layout_set_width(layout, nameWidth * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
        pango_layout_set_single_paragraph_mode(layout, TRUE);
        cairo_move_to(cr, nameLeft, y + 3);
        pango_cairo_show_layout(cr, layout);

        const std::string size = humanBytes(sizeOf(row.node));
        pango_layout_set_text(layout, size.c_str(), -1);
        pango_layout_set_width(layout, (kSizeColumn - 10) * PANGO_SCALE);
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
        cairo_move_to(cr, width - kSizeColumn, y + 3);
        pango_cairo_show_layout(cr, layout);
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    }

    pango_font_description_free(font);
    g_object_unref(layout);
}

int TreePane::rowAt(double y) const {
    const int index = int(y) / kRowHeight;
    if (index < 0 || size_t(index) >= rows_.size()) return -1;
    return index;
}

void TreePane::pressTrampoline(GtkGestureClick*, int count, double x, double y, void* data) {
    auto* self = static_cast<TreePane*>(data);
    const int index = self->rowAt(y);
    if (index < 0) return;
    const Row& row = self->rows_[size_t(index)];

    // The expander's own column toggles rather than selects, so opening a folder
    // does not also move the selection out from under the pointer.
    const double indent = 6 + row.depth * kIndent;
    if (row.hasChildren && x >= indent && x < indent + 12) {
        if (self->expanded_.count(row.node)) {
            self->expanded_.erase(row.node);
        } else {
            self->expanded_.insert(row.node);
        }
        self->rebuild();
        return;
    }

    const int32_t node = row.node;
    self->selected_ = node;
    gtk_widget_queue_draw(self->area_);
    if (count >= 2) {
        if (self->expanded_.count(node)) {
            self->expanded_.erase(node);
        } else {
            self->expanded_.insert(node);
        }
        self->rebuild();
        if (self->onActivated) self->onActivated(node);
        return;
    }
    if (self->onSelected) self->onSelected(node);
}

void TreePane::selectNode(int32_t node) {
    if (selected_ == node) return;
    selected_ = node;
    gtk_widget_queue_draw(area_);
}

void TreePane::scrollToRow(int index) {
    if (index < 0) return;
    GtkAdjustment* adjustment =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double top = double(index) * kRowHeight;
    const double page = gtk_adjustment_get_page_size(adjustment);
    const double value = gtk_adjustment_get_value(adjustment);
    if (top < value) {
        gtk_adjustment_set_value(adjustment, top);
    } else if (top + kRowHeight > value + page) {
        gtk_adjustment_set_value(adjustment, top + kRowHeight - page);
    }
}

void TreePane::revealNode(int32_t node) {
    if (!arena_ || node < 0) return;

    // Every ancestor has to be open for the row to exist at all. Walked upwards
    // and opened on the way, which is cheap: the chain is the depth of the tree,
    // not the size of it.
    std::vector<int32_t> chain;
    size_t current = size_t(uint32_t(node));
    while (current != size_t(uint32_t(root_)) && current > 0) {
        chain.push_back(int32_t(current));
        current = size_t(uint32_t(arena_->parent[current]));
    }
    for (size_t i = chain.size(); i-- > 1;) expanded_.insert(chain[i]);
    expanded_.insert(root_);
    rebuild();

    selected_ = node;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].node == node) {
            scrollToRow(int(i));
            break;
        }
    }
    gtk_widget_queue_draw(area_);
}

}  // namespace app
