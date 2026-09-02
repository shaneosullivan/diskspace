#include "kind_pane.h"

#include <algorithm>
#include <string>

#include "file_kind.h"
#include "format.h"

namespace app {
namespace {

constexpr int kSizeColumnBase = 90;
constexpr int kFilesColumnBase = 70;

}  // namespace

int KindPane::headerHeight(const Context& context) const {
    return context.metrics.rowHeight;
}

void KindPane::columns(const Context& context, int* swatch, int* name, int* size,
                       int* files) const {
    const double scale = double(context.metrics.rowHeight) / 22.0;
    const int sizeWidth = int(kSizeColumnBase * scale);
    const int filesWidth = int(kFilesColumnBase * scale);
    const int right = bounds_.right() - (scrollbar_.visible() ? Scrollbar::kWidth : 0);
    *swatch = bounds_.x + 8;
    *name = bounds_.x + 24;
    *files = right - filesWidth - 8;
    *size = *files - sizeWidth;
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
}

int KindPane::preferredHeight(const Context& context) const {
    return headerHeight(context) + int(rows_.size()) * context.metrics.rowHeight;
}

void KindPane::draw(ui::Canvas& canvas, const Context& context) {
    const Theme& theme = context.theme;
    canvas.fill(bounds_, theme.well);

    const int rowHeight = context.metrics.rowHeight;
    const int header = headerHeight(context);
    const int viewport = bounds_.height - header;
    scrollbar_.setGeometry(ui::Rect{bounds_.right() - Scrollbar::kWidth, bounds_.y + header,
                                    Scrollbar::kWidth, std::max(0, viewport)},
                           int(rows_.size()) * rowHeight, std::max(0, viewport));

    int swatchX, nameX, sizeX, filesX;
    columns(context, &swatchX, &nameX, &sizeX, &filesX);
    const double scale = double(rowHeight) / 22.0;
    const int sizeWidth = int(kSizeColumnBase * scale);
    const int filesWidth = int(kFilesColumnBase * scale);

    // The headings, with an arrow on whichever column the rows are ordered by.
    auto heading = [&](const char* label, int x, int columnWidth, ui::Align align, SortKey key) {
        std::string value = label;
        if (sortKey_ == key) value += ascending_ ? "  \xe2\x96\xb4" : "  \xe2\x96\xbe";
        canvas.textIn(ui::Rect{x, bounds_.y, columnWidth, header}, value, context.fonts.small,
                      theme.dimText, align);
    };
    heading("Kind", nameX, sizeX - nameX - 8, ui::Align::Left, SortKey::Kind);
    heading("Size", sizeX, sizeWidth - 8, ui::Align::Right, SortKey::Size);
    heading("Files", filesX, filesWidth, ui::Align::Right, SortKey::Files);
    canvas.fill(ui::Rect{bounds_.x, bounds_.y + header - 1, bounds_.width, 1}, theme.border);

    canvas.pushClip(ui::Rect{bounds_.x, bounds_.y + header, bounds_.width, std::max(0, viewport)});

    const std::vector<diskscan::NameString>& names = diskscan::FileKind::names();
    const int offset = scrollbar_.offset();
    const int firstRow = std::max(0, offset / rowHeight);
    const int lastRow = std::min(int(rows_.size()), (offset + viewport) / rowHeight + 2);

    for (int i = firstRow; i < lastRow; ++i) {
        const diskscan::TreeScanner::KindTotal& row = rows_[size_t(i)];
        const int y = bounds_.y + header + i * rowHeight - offset;
        const bool isSelected = row.kind == selected_;
        const ui::Rect rowRect{bounds_.x, y, bounds_.width, rowHeight};

        if (isSelected) {
            canvas.fill(rowRect, theme.selection);
        } else if (rowRect.contains(context.pointer.x, context.pointer.y)) {
            canvas.fill(rowRect, theme.dark ? ui::rgba(0xff, 0xff, 0xff, 0x0e)
                                            : ui::rgba(0, 0, 0, 0x0a));
        }

        if (palette_) {
            const uint32_t colour = palette_->colour(row.kind);
            canvas.fill(ui::Rect{swatchX, y + rowHeight / 2 - 4, 9, 9}, 0xFF000000u | colour);
        }

        const size_t index = size_t(uint32_t(row.kind));
        const std::string name = index < names.size() ? names[index] : std::string("Other");
        const ui::Color colour = isSelected ? theme.selectionText : theme.text;
        canvas.textIn(ui::Rect{nameX, y, sizeX - nameX - 8, rowHeight}, name, context.fonts.small,
                      colour);
        canvas.textIn(ui::Rect{sizeX, y, sizeWidth - 8, rowHeight}, humanBytes(row.bytes),
                      context.fonts.small, colour, ui::Align::Right);
        canvas.textIn(ui::Rect{filesX, y, filesWidth, rowHeight}, grouped(row.count),
                      context.fonts.small, colour, ui::Align::Right);
    }

    canvas.popClip();
    scrollbar_.draw(canvas, theme, context.pointer);
}

bool KindPane::mousePress(int x, int y, int button, const Context& context) {
    if (!bounds_.contains(x, y) || button != 1) return false;
    if (scrollbar_.press(x, y)) {
        redraw();
        return true;
    }

    int swatchX, nameX, sizeX, filesX;
    columns(context, &swatchX, &nameX, &sizeX, &filesX);
    const int header = headerHeight(context);

    if (y < bounds_.y + header) {
        // A heading. Clicking the one already sorted by reverses it, which is what
        // every table anyone has used does.
        const SortKey key = x >= filesX  ? SortKey::Files
                            : x >= sizeX ? SortKey::Size
                                         : SortKey::Kind;
        if (sortKey_ == key) {
            ascending_ = !ascending_;
        } else {
            sortKey_ = key;
            // Size and count read biggest first; a name reads A to Z.
            ascending_ = key == SortKey::Kind;
        }
        applySort();
        redraw();
        return true;
    }

    const int index = (y - bounds_.y - header + scrollbar_.offset()) / context.metrics.rowHeight;
    if (index < 0 || size_t(index) >= rows_.size()) return true;
    const int16_t kind = rows_[size_t(index)].kind;
    selected_ = selected_ == kind ? int16_t(-1) : kind;
    redraw();
    if (onSelected) onSelected(selected_);
    return true;
}

bool KindPane::mouseDrag(int x, int y) {
    if (!scrollbar_.dragging()) return false;
    if (scrollbar_.drag(y)) redraw();
    return true;
}

bool KindPane::scroll(int x, int y, int steps, const Context& context) {
    if (!bounds_.contains(x, y)) return false;
    scrollbar_.scrollBy(steps * context.metrics.rowHeight * 3);
    redraw();
    return true;
}

}  // namespace app
