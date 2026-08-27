#pragma once

#include <gtk/gtk.h>

namespace app {

/// The settings window: one switch per preference, grouped by what it affects.
///
/// Presented rather than owned - it closes itself, and every switch writes
/// through to Preferences immediately, which notifies the main window. There is
/// no OK button because there is nothing to confirm.
void presentPreferences(GtkWindow* parent);

}  // namespace app
