#pragma once

#include <gtk/gtk.h>

namespace app {

/// The few colours the custom-drawn panes need from the theme.
///
/// GTK 4.10 deprecated looking colours up by name out of a style context, and
/// offered no replacement for arbitrary names - the theme's palette is no longer
/// a public dictionary. What is still public is the colour the widget would draw
/// its own text in, so that is the one thing asked for, and everything else is
/// derived from it. A light foreground means a dark theme, which is the only
/// other fact these panes need.

/// The text colour for this widget, from the theme.
GdkRGBA foreground(GtkWidget* widget);

/// Whether the theme is a dark one, judged by the foreground being light.
bool isDark(GtkWidget* widget);

/// The colour behind a selected row. Not the theme's own accent, which is not
/// reachable any more; a blue that holds up against both light and dark text.
GdkRGBA selection(GtkWidget* widget);

}  // namespace app
