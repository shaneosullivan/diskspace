#pragma once

#include "x11.h"

namespace app {

/// The colours everything is drawn in.
///
/// GTK used to answer this. Without it there is nobody to ask, so the desktop's
/// own configuration is read instead: whether the user has chosen a dark theme
/// is a fact recorded in a file, and reading it is cheaper than pretending the
/// question does not exist.
///
/// The palette itself is this application's, not the desktop's. Matching a GTK
/// theme's exact greys was never possible from outside it, and half-matching
/// looks worse than a coherent palette of one's own - which is what the macOS
/// and Windows builds do too.
struct Theme {
    bool dark = false;

    ui::Color background = 0;
    /// Panels, cards, the header: one step away from the background.
    ui::Color surface = 0;
    ui::Color surfaceHover = 0;
    ui::Color surfacePressed = 0;
    ui::Color border = 0;

    ui::Color text = 0;
    ui::Color dimText = 0;
    ui::Color selection = 0;
    ui::Color selectionText = 0;
    ui::Color accent = 0;

    /// Behind the treemap and the list, which want to be a shade deeper than the
    /// chrome around them.
    ui::Color well = 0;

    /// Read the desktop's preference and build the matching palette.
    static Theme detect();
    static Theme light();
    static Theme darkTheme();
};

}  // namespace app
