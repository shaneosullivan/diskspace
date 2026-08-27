#include "kind_pane.h"

#include <algorithm>
#include <string>

#include "file_kind.h"
#include "format.h"
#include "theme.h"

namespace app {

KindPane::KindPane() {
    area_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_),
                                   GtkDrawingAreaDrawFunc(&KindPane::drawTrampoline), this,
                                   nullptr);

    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(&KindPane::pressTrampoline), this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(click));

    scroller_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller_), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), area_);
    gtk_widget_set_vexpand(scroller_, TRUE);
}

void KindPane::columns(int width, int* swatch, int* name, int* size, int* files) const {
    *swatch = 8;
    *name = 24;
    *files = width - kFilesColumn - 8;
    *size = *files - kSizeColumn;
}

void KindPane::applySort() {
    const std::vector<diskscan::NameString>& names = diskscan::FileKind::names();
    std::sort(rows_.begin(), rows_.end(),
              [&](const diskscan::TreeScanner::KindTotal& a,
                  const diskscan::TreeScanner::KindTotal& b) {
                  bool less;
                  switch (sortKey_) {
                      case SortKey::Kind: {
                          const size_t ai = size_t(uint32_t(a.kind));
                          const size_t bi = size_t(uint32_t(b.kind));
                          const diskscan::NameString& an =
                              ai < names.size() ? names[ai] : diskscan::NameString();
                          const diskscan::NameString& bn =
                              bi < names.size() ? names[bi] : diskscan::NameString();
                          less = an < bn;
                          break;
                      }
                      case SortKey::Files: less = a.count < b.count; break;
                      default: less = a.bytes < b.bytes; break;
                  }
                  return ascending_ ? less : !less;
              });
}

void KindPane::refresh() {
    rows_.clear();
    if (scanner_) rows_ = scanner_->kindTotals(metric_, useLogicalSize_);
    // Kinds that occupy nothing under the current measure are noise: switching to
    // "added last month" would otherwise leave a page of zeroes.
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
                               [](const diskscan::TreeScanner::KindTotal& row) {
                                   return row.bytes <= 0;
                               }),
                rows_.end());
    applySort();
    gtk_widget_set_size_request(area_, -1, kHeaderHeight + int(rows_.size()) * kRowHeight);
    gtk_widget_queue_draw(area_);
}

void KindPane::drawTrampoline(GtkDrawingArea*, cairo_t* cr, int width, int height, void* self) {
    static_cast<KindPane*>(self)->draw(cr, width, height);
}

void KindPane::draw(cairo_t* cr, int width, int height) {
    (void)height;
    const GdkRGBA text = foreground(area_);
    const GdkRGBA accent = selection(area_);

    int swatchX, nameX, sizeX, filesX;
    columns(width, &swatchX, &nameX, &sizeX, &filesX);

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string("Sans 9");
    pango_layout_set_font_description(layout, font);

    auto drawText = [&](const std::string& value, int x, int y, int columnWidth,
                        PangoAlignment alignment, double alpha) {
        cairo_set_source_rgba(cr, text.red, text.green, text.blue, alpha);
        pango_layout_set_text(layout, value.c_str(), -1);
        pango_layout_set_width(layout, columnWidth * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_alignment(layout, alignment);
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, layout);
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    };

    // The headings, with an arrow on whichever column the rows are ordered by.
    auto heading = [&](const char* label, int x, int columnWidth, PangoAlignment alignment,
                       SortKey key) {
        std::string value = label;
        if (sortKey_ == key) value += ascending_ ? "  \xe2\x96\xb4" : "  \xe2\x96\xbe";
        drawText(value, x, 3, columnWidth, alignment, 0.6);
    };
    heading("Kind", nameX, sizeX - nameX - 8, PANGO_ALIGN_LEFT, SortKey::Kind);
    heading("Size", sizeX, kSizeColumn - 8, PANGO_ALIGN_RIGHT, SortKey::Size);
    heading("Files", filesX, kFilesColumn, PANGO_ALIGN_RIGHT, SortKey::Files);

    cairo_set_source_rgba(cr, text.red, text.green, text.blue, 0.18);
    cairo_rectangle(cr, 0, kHeaderHeight - 1, width, 1);
    cairo_fill(cr);

    const std::vector<diskscan::NameString>& names = diskscan::FileKind::names();
    double clipX1, clipY1, clipX2, clipY2;
    cairo_clip_extents(cr, &clipX1, &clipY1, &clipX2, &clipY2);
    const int firstRow = std::max(0, (int(clipY1) - kHeaderHeight) / kRowHeight);
    const int lastRow =
        std::min(int(rows_.size()), (int(clipY2) - kHeaderHeight) / kRowHeight + 2);

    for (int i = firstRow; i < lastRow; ++i) {
        const diskscan::TreeScanner::KindTotal& row = rows_[size_t(i)];
        const double y = kHeaderHeight + double(i) * kRowHeight;
        const bool isSelected = row.kind == selected_;

        if (isSelected) {
            gdk_cairo_set_source_rgba(cr, &accent);
            cairo_rectangle(cr, 0, y, width, kRowHeight);
            cairo_fill(cr);
        }

        if (palette_) {
            const uint32_t colour = palette_->colour(row.kind);
            cairo_set_source_rgb(cr, ((colour >> 16) & 0xFF) / 255.0,
                                 ((colour >> 8) & 0xFF) / 255.0, (colour & 0xFF) / 255.0);
            cairo_rectangle(cr, swatchX, y + kRowHeight / 2.0 - 4, 9, 9);
            cairo_fill(cr);
        }

        const size_t index = size_t(uint32_t(row.kind));
        const std::string name = index < names.size() ? names[index] : std::string("Other");
        const double alpha = isSelected ? 1.0 : 0.92;
        if (isSelected) cairo_set_source_rgb(cr, 1, 1, 1);
        drawText(name, nameX, int(y) + 2, sizeX - nameX - 8, PANGO_ALIGN_LEFT, alpha);
        drawText(humanBytes(row.bytes), sizeX, int(y) + 2, kSizeColumn - 8, PANGO_ALIGN_RIGHT,
                 alpha);
        drawText(grouped(row.count), filesX, int(y) + 2, kFilesColumn, PANGO_ALIGN_RIGHT,
                 alpha);
    }

    pango_font_description_free(font);
    g_object_unref(layout);
}

void KindPane::pressTrampoline(GtkGestureClick*, int, double x, double y, void* data) {
    auto* self = static_cast<KindPane*>(data);
    const int width = gtk_widget_get_width(self->area_);
    int swatchX, nameX, sizeX, filesX;
    self->columns(width, &swatchX, &nameX, &sizeX, &filesX);

    if (y < kHeaderHeight) {
        // A heading. Clicking the one already sorted by reverses it, which is what
        // every table anyone has used does.
        const SortKey key = x >= filesX  ? SortKey::Files
                            : x >= sizeX ? SortKey::Size
                                         : SortKey::Kind;
        if (self->sortKey_ == key) {
            self->ascending_ = !self->ascending_;
        } else {
            self->sortKey_ = key;
            // Size and count read biggest first; a name reads A to Z.
            self->ascending_ = key == SortKey::Kind;
        }
        self->applySort();
        gtk_widget_queue_draw(self->area_);
        return;
    }

    const int index = (int(y) - kHeaderHeight) / kRowHeight;
    if (index < 0 || size_t(index) >= self->rows_.size()) return;
    const int16_t kind = self->rows_[size_t(index)].kind;
    self->selected_ = self->selected_ == kind ? int16_t(-1) : kind;
    gtk_widget_queue_draw(self->area_);
    if (self->onSelected) self->onSelected(self->selected_);
}

}  // namespace app
