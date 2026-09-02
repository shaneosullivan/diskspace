#include "preferences_window.h"

#include <X11/keysym.h>

#include <algorithm>

#include "preferences.h"

namespace app {

const std::vector<PreferencesPanel::Row>& PreferencesPanel::rows() {
    // Function pointers rather than lambdas with captures, so the table can be a
    // static constant: every one of these is a getter and a setter on the single
    // shared Preferences, and there is nothing to capture.
    static const std::vector<Row> table = {
        {"Show space occupied on disk",
         "Rather than the length of a file's contents. The two differ where sparse "
         "files and compression are involved.",
         [] { return Preferences::shared().showPhysicalFileSize(); },
         [](bool v) { Preferences::shared().setShowPhysicalFileSize(v); }},
        {"Put the list above the map",
         "Rather than beside it.",
         [] { return Preferences::shared().splitHorizontally(); },
         [](bool v) { Preferences::shared().setSplitHorizontally(v); }},
        {"Consistent colours per file kind",
         "A file type looks the same in every scan. Off, the biggest kinds in this "
         "scan get the most distinct hues.",
         [] { return Preferences::shared().consistentKindColours(); },
         [](bool v) { Preferences::shared().setConsistentKindColours(v); }},
        {"Show free space",
         "Draw the volume's free space as a block, to size the scanned folder "
         "against the room left on it.",
         [] { return Preferences::shared().showFreeSpace(); },
         [](bool v) { Preferences::shared().setShowFreeSpace(v); }},
        {"Show other space in use",
         "Everything else on the volume, as a block. Nothing to show when a whole "
         "volume is on screen.",
         [] { return Preferences::shared().showOtherSpace(); },
         [](bool v) { Preferences::shared().setShowOtherSpace(v); }},
        {"Show the file kinds table",
         "",
         [] { return Preferences::shared().showFileKinds(); },
         [](bool v) { Preferences::shared().setShowFileKinds(v); }},
        {"Check for updates at launch",
         "The only network request this program makes. It carries nothing about "
         "what was scanned.",
         [] { return Preferences::shared().checkForUpdates(); },
         [](bool v) { Preferences::shared().setCheckForUpdates(v); }},
    };
    return table;
}

void PreferencesPanel::draw(ui::Canvas& canvas, const Context& context) {
    if (!open_) return;
    const Theme& theme = context.theme;
    canvas.fill(ui::Rect{0, 0, canvas.width(), canvas.height()}, ui::rgba(0, 0, 0, 0x66));

    const int rowHeight = context.metrics.rowHeight;
    const int padding = context.metrics.padding;
    const int entryHeight = rowHeight * 2 + 6;

    const int width = std::min(canvas.width() - 60, rowHeight * 28);
    const int height =
        std::min(canvas.height() - 40,
                 padding * 2 + rowHeight + 12 + int(rows().size()) * entryHeight + rowHeight + 14);
    panel_ = ui::Rect{(canvas.width() - width) / 2, (canvas.height() - height) / 2, width, height};

    canvas.fillRounded(panel_, 10, theme.background);
    canvas.frame(panel_, theme.border);

    int y = panel_.y + padding;
    canvas.textIn(ui::Rect{panel_.x + padding + 4, y, panel_.width - 2 * padding, rowHeight + 6},
                  "Settings", context.fonts.title, theme.text);
    y += rowHeight + 12;

    rowBounds_.clear();
    for (const Row& row : rows()) {
        const ui::Rect bounds{panel_.x + padding, y, panel_.width - 2 * padding, entryHeight};
        rowBounds_.push_back(bounds);

        const bool hovered = bounds.contains(context.pointer.x, context.pointer.y);
        if (hovered) canvas.fillRounded(bounds, 6, theme.surfaceHover);

        // The switch: a rounded track with a knob at one end.
        const int trackWidth = rowHeight + 14;
        const int trackHeight = rowHeight - 4;
        const ui::Rect track{bounds.right() - trackWidth - 6, bounds.y + 6, trackWidth,
                             trackHeight};
        const bool on = row.get();
        canvas.fillRounded(track, trackHeight / 2, on ? theme.accent : theme.border);
        const int knob = trackHeight - 4;
        canvas.fillRounded(ui::Rect{on ? track.right() - knob - 2 : track.x + 2, track.y + 2, knob,
                                    knob},
                           knob / 2, ui::rgb(0xff, 0xff, 0xff));

        canvas.textIn(ui::Rect{bounds.x + 8, bounds.y + 2, bounds.width - trackWidth - 24,
                               rowHeight},
                      row.label, context.fonts.regular, theme.text);
        if (!row.detail.empty()) {
            canvas.textIn(ui::Rect{bounds.x + 8, bounds.y + rowHeight, bounds.width - 24, rowHeight},
                          row.detail, context.fonts.small, theme.dimText);
        }
        y += entryHeight;
    }

    y = panel_.bottom() - padding - rowHeight - 6;
    resetButton_.label = "Reset All";
    closeButton_.label = "Done";
    const int closeWidth = closeButton_.preferredWidth(canvas, context.fonts.regular) + 16;
    const int resetWidth = resetButton_.preferredWidth(canvas, context.fonts.regular) + 16;
    closeButton_.bounds =
        ui::Rect{panel_.right() - padding - closeWidth, y, closeWidth, rowHeight + 6};
    resetButton_.bounds = ui::Rect{panel_.x + padding, y, resetWidth, rowHeight + 6};
    resetButton_.draw(canvas, theme, context.pointer, context.fonts.regular);
    closeButton_.draw(canvas, theme, context.pointer, context.fonts.regular);
}

bool PreferencesPanel::mousePress(int x, int y, int button, const Context& context) {
    if (!open_) return false;
    if (button != 1) return true;

    if (!panel_.contains(x, y)) {
        open_ = false;
        redraw();
        return true;
    }
    if (closeButton_.hit(x, y)) {
        open_ = false;
        redraw();
        return true;
    }
    if (resetButton_.hit(x, y)) {
        Preferences::shared().resetAll();
        redraw();
        return true;
    }
    for (size_t i = 0; i < rowBounds_.size() && i < rows().size(); ++i) {
        if (rowBounds_[i].contains(x, y)) {
            // The whole row toggles, not just the switch: a two-line row with a
            // small target at one end is a needlessly precise thing to ask for.
            rows()[i].set(!rows()[i].get());
            redraw();
            return true;
        }
    }
    return true;
}

bool PreferencesPanel::key(unsigned long keysym) {
    if (!open_) return false;
    if (keysym == XK_Escape || keysym == XK_Return) {
        open_ = false;
        redraw();
    }
    return true;
}

}  // namespace app
