// Disk Space for Linux: the same scan engine as the Windows and macOS builds,
// behind GTK4.

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

#include "platform.h"
#include "product.h"
#include "window.h"

namespace {

/// The window, kept alive for the life of the application.
std::unique_ptr<app::MainWindow> gWindow;
/// A path from the command line, scanned as soon as there is a window to show it
/// in.
std::string gStartPath;

void activate(GtkApplication* application, gpointer) {
    if (!gWindow) gWindow = std::make_unique<app::MainWindow>(application);
    gtk_window_present(GTK_WINDOW(gWindow->widget()));
    if (!gStartPath.empty()) {
        gWindow->scanPath(gStartPath);
        gStartPath.clear();
    }
}

/// GApplication owns argument parsing so that a second invocation hands its
/// arguments to the running instance rather than starting a rival window.
int commandLine(GApplication* application, GApplicationCommandLine* commandLine, gpointer) {
    int count = 0;
    char** argv = g_application_command_line_get_arguments(commandLine, &count);
    for (int i = 1; i < count; ++i) {
        if (argv[i][0] == '-') continue;
        gStartPath = argv[i];
        break;
    }
    g_strfreev(argv);
    g_application_activate(application);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    GtkApplication* application =
        gtk_application_new(DISKSPACE_APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(application, "activate", G_CALLBACK(activate), nullptr);
    g_signal_connect(application, "command-line", G_CALLBACK(commandLine), nullptr);

    const int status = g_application_run(G_APPLICATION(application), argc, argv);
    gWindow.reset();
    g_object_unref(application);
    return status;
}
