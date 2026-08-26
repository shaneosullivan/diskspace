#pragma once

// The one place the version is set.
//
// app.rc compiles these into the executable's version resource, which is what
// UpdateCheck reports to the site and compares against what the site answers.
// The packaging script reads the same numbers and stamps them into the MSIX
// manifest, so a package and the executable inside it can never disagree.
//
// Bump this for a release. The Store additionally requires that each submission
// carry a higher version than the last.

#define DISKSPACE_VERSION_MAJOR 1
#define DISKSPACE_VERSION_MINOR 0
#define DISKSPACE_VERSION_PATCH 0
// The Store reserves the fourth component and requires it to be zero.
#define DISKSPACE_VERSION_REVISION 0

#define DISKSPACE_STRINGIFY_(x) #x
#define DISKSPACE_STRINGIFY(x) DISKSPACE_STRINGIFY_(x)

/// "1.0.0" — three components, which is the form the site publishes releases in.
#define DISKSPACE_VERSION_STRING                                       \
    DISKSPACE_STRINGIFY(DISKSPACE_VERSION_MAJOR)                       \
    "." DISKSPACE_STRINGIFY(DISKSPACE_VERSION_MINOR) "." DISKSPACE_STRINGIFY( \
        DISKSPACE_VERSION_PATCH)

/// "1.0.0.0" — four components, which is what a Windows version resource and an
/// MSIX identity both want.
#define DISKSPACE_VERSION_STRING_FULL \
    DISKSPACE_VERSION_STRING "." DISKSPACE_STRINGIFY(DISKSPACE_VERSION_REVISION)
