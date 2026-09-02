// Disk Space for Linux: the same scan engine as the Windows and macOS builds,
// behind Xlib and Xft rather than a toolkit. See docs/linux-x11-plan.md.

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "product.h"
#include "window.h"
#include "x11.h"

namespace {

/// The X selection that says "an instance of this application is already
/// running, and here is its window".
///
/// GApplication used to do this. Without it the handoff is a named selection: the
/// first instance claims it, and a second one finds the owner, hands over its
/// argument as a window property and asks the window manager to raise it. Which
/// is all GApplication was doing underneath, minus the bus.
constexpr const char* kInstanceSelection = "_DISKSPACE_INSTANCE";
constexpr const char* kOpenPathProperty = "_DISKSPACE_OPEN_PATH";

/// Hand `path` to the running instance and raise it. False when there is none.
bool handOffToRunningInstance(const std::string& path) {
    ::Display* display = XOpenDisplay(nullptr);
    if (!display) return false;

    const Atom selection = XInternAtom(display, kInstanceSelection, False);
    const ::Window owner = XGetSelectionOwner(display, selection);
    if (owner == None) {
        XCloseDisplay(display);
        return false;
    }

    if (!path.empty()) {
        XChangeProperty(display, owner, XInternAtom(display, kOpenPathProperty, False),
                        XInternAtom(display, "UTF8_STRING", False), 8, PropModeReplace,
                        (const unsigned char*)path.data(), int(path.size()));
    }

    // _NET_ACTIVE_WINDOW rather than XRaiseWindow: a window manager is entitled
    // to ignore a raise from a client it did not focus, and this is the message
    // it does honour. Source 2 means "from a pager or another application",
    // which is what this is.
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = owner;
    event.xclient.message_type = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
    XCloseDisplay(display);
    return true;
}

void printUsage() {
    std::printf("%s %s\n", DISKSPACE_APP_NAME, DISKSPACE_VERSION_STRING);
    std::printf("usage: diskspace [path]\n");
    std::printf("  path    a folder to scan at once, instead of the chooser\n\n");
    std::printf("  --capture FILE        draw one frame to a PPM file and exit\n");
    std::printf("  --capture-after SEC   how long to wait first (default 1)\n\n");
    std::printf("Environment:\n");
    std::printf("  DISKSPACE_THEME=light|dark   override the desktop's setting\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string startPath;
    std::string capturePath;
    double captureAfter = 1.0;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--version") {
            std::printf("%s\n", DISKSPACE_VERSION_STRING);
            return 0;
        }
        if (argument == "--help" || argument == "-h") {
            printUsage();
            return 0;
        }
        if (argument == "--capture" && i + 1 < argc) {
            capturePath = argv[++i];
            continue;
        }
        if (argument == "--capture-after" && i + 1 < argc) {
            captureAfter = std::atof(argv[++i]);
            continue;
        }
        if (!argument.empty() && argument[0] != '-') startPath = argument;
    }

    // A second launch hands its argument to the first and stops, so opening a
    // folder from a file manager twice does not leave two windows behind. Not
    // when a picture was asked for: that is a fresh window every time, or the
    // screenshot would be of whatever was already on screen.
    if (capturePath.empty() && handOffToRunningInstance(startPath)) return 0;

    ui::Display* display = ui::Display::open();
    if (!display) {
        std::fprintf(stderr,
                     "Cannot open a connection to the X display.\n"
                     "This is a graphical application; DISPLAY has to be set.\n"
                     "On a Wayland-only session it reaches the screen through XWayland.\n");
        return 1;
    }

    auto window = std::make_unique<app::MainWindow>(display);
    if (!window->create()) {
        std::fprintf(stderr, "Could not create the window.\n");
        return 1;
    }

    // Claimed after the window exists, so that anything handing over a path has
    // somewhere to put it. Not claimed by a run that is only taking a picture,
    // which should neither steal the handoff from a real instance nor be handed
    // anything itself.
    if (capturePath.empty()) {
        XSetSelectionOwner(display->xdisplay(),
                           XInternAtom(display->xdisplay(), kInstanceSelection, False),
                           window->window().handle(), CurrentTime);
    }

    if (!startPath.empty()) window->scanPath(startPath);
    if (!capturePath.empty()) window->captureAndExit(capturePath, captureAfter);
    window->run();

    window.reset();
    delete display;
    return 0;
}
