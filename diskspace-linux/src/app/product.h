#pragma once

// What this build calls itself, and which version it is. One place, because
// these appear in the window title, the About box, the desktop entry and the
// Flatpak manifest, and any of them disagreeing is the kind of thing nobody
// notices until it is in front of a user.

// Plain "Disk Space" here. The Windows build is "Disk Space ++" only because the
// name was already taken on the Microsoft Store; nothing on Linux takes it away,
// so this matches the macOS build and what the site calls the application.
#define DISKSPACE_APP_NAME "Disk Space"

// The reverse-DNS identifier, which is what a desktop file, an icon and a
// Flatpak are all keyed on.
#define DISKSPACE_APP_ID "com.chofter.DiskSpace"

// Bump for a release.
#define DISKSPACE_VERSION_MAJOR 1
#define DISKSPACE_VERSION_MINOR 0
#define DISKSPACE_VERSION_PATCH 0

#define DISKSPACE_STRINGIFY_(x) #x
#define DISKSPACE_STRINGIFY(x) DISKSPACE_STRINGIFY_(x)

/// "1.0.0" - three components, which is the form the site publishes releases in.
#define DISKSPACE_VERSION_STRING                                                    \
    DISKSPACE_STRINGIFY(DISKSPACE_VERSION_MAJOR)                                    \
    "." DISKSPACE_STRINGIFY(DISKSPACE_VERSION_MINOR) "." DISKSPACE_STRINGIFY(       \
        DISKSPACE_VERSION_PATCH)
