#pragma once

// What this build calls itself, and which version it is. One place, because
// these appear in the window title, the About box, the executable's version
// resource and the MSIX manifest, and any of them disagreeing is the kind of
// thing nobody notices until it is in front of a user.
//
// app.rc compiles them into the version resource, which is also what
// UpdateCheck reports to the site. The packaging script reads the same file, so
// a package and the executable inside it can never disagree.

// The Windows name differs from the macOS one: "Disk Space" was already taken on
// the Microsoft Store, so this build is "Disk Space ++". The two applications
// are otherwise the same thing, and the site publishes them under one name.
#define DISKSPACE_APP_NAME "Disk Space ++"

// Bump for a release. The Store additionally requires each submission to carry a
// higher version than the last.
#define DISKSPACE_VERSION_MAJOR 1
#define DISKSPACE_VERSION_MINOR 0
#define DISKSPACE_VERSION_PATCH 0
// The Store reserves the fourth component and requires it to be zero.
#define DISKSPACE_VERSION_REVISION 0

#define DISKSPACE_STRINGIFY_(x) #x
#define DISKSPACE_STRINGIFY(x) DISKSPACE_STRINGIFY_(x)
#define DISKSPACE_WIDEN_(x) L##x
#define DISKSPACE_WIDEN(x) DISKSPACE_WIDEN_(x)

/// The name as a wide string, which is what every Win32 call that shows it wants.
#define DISKSPACE_APP_NAME_W DISKSPACE_WIDEN(DISKSPACE_APP_NAME)

/// "1.0.0" — three components, which is the form the site publishes releases in.
#define DISKSPACE_VERSION_STRING                                       \
    DISKSPACE_STRINGIFY(DISKSPACE_VERSION_MAJOR)                       \
    "." DISKSPACE_STRINGIFY(DISKSPACE_VERSION_MINOR) "." DISKSPACE_STRINGIFY( \
        DISKSPACE_VERSION_PATCH)

/// "1.0.0.0" — four components, which is what a Windows version resource and an
/// MSIX identity both want.
#define DISKSPACE_VERSION_STRING_FULL \
    DISKSPACE_VERSION_STRING "." DISKSPACE_STRINGIFY(DISKSPACE_VERSION_REVISION)
