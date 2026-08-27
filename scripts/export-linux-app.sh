#!/usr/bin/env bash
# =============================================================================
# export-linux-app.sh
#
# Builds, verifies and packages the Linux release of Disk Space.
#
# Usage:
#   ./scripts/export-linux-app.sh
#   ./scripts/export-linux-app.sh --skip-tests    when the suite has just run
#
# The counterpart of export-macos-app.sh and export-windows-app.ps1, and the
# shortest of the three, because Linux asks for neither a certificate nor a
# notary service. What it does insist on is that the thing being shipped was
# actually tested, so the accuracy suite runs against the binary being packaged
# rather than against whatever happened to be built last.
#
# Two artefacts come out:
#
#   * An AppImage: one file, nothing to install. Built only if appimagetool is on
#     the path, and the AppDir is produced either way.
#   * A tarball of the same tree, for anyone who would rather have the binaries.
#
# Neither bundles GTK, so unlike the portable Windows .exe - which links the C
# runtime statically and needs nothing at all - both require GTK 4.10 or later on
# the machine that runs them. Every current distribution has it; a two-year-old
# one does not. Bundling it properly means linuxdeploy with its GTK plugin, which
# drags in something like eighty megabytes of libraries, and that is a trade to
# make deliberately rather than by default.
#
# Prerequisites:
#   - build-essential, cmake, ninja-build, pkg-config, libgtk-4-dev
#   - appimagetool, optionally, for the single-file build. Not in the Ubuntu
#     archive; it comes from the AppImageKit releases page, and being an
#     AppImage itself it needs libfuse2 to run - or --appimage-extract-and-run
#     where FUSE is unavailable, which is the usual case in a container.
# =============================================================================

set -euo pipefail

SKIP_TESTS=0
for argument in "$@"; do
    case "$argument" in
        --skip-tests) SKIP_TESTS=1 ;;
        *)
            echo "unknown option: $argument" >&2
            echo "usage: $0 [--skip-tests]" >&2
            exit 2
            ;;
    esac
done

# =============================================================================
# PATHS
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LINUX_DIR="$ROOT/diskspace-linux"
BUILD_DIR="$ROOT/build/linux"
RELEASE_DIR="$ROOT/build/linux-release"

APP_ID="com.chofter.DiskSpace"

echo "=== Disk Space — Linux Export ==="
echo

# =============================================================================
# VERSION
# =============================================================================
#
# From product.h, which is where it is bumped, so the binary and everything
# named after it can never disagree about what they are.

PRODUCT_H="$LINUX_DIR/src/app/product.h"

versionPart() {
    local name="$1"
    local value
    value="$(sed -n "s/^#define DISKSPACE_VERSION_${name}[[:space:]]\+\([0-9]\+\).*/\1/p" \
             "$PRODUCT_H")"
    if [ -z "$value" ]; then
        echo "could not read DISKSPACE_VERSION_${name} from $PRODUCT_H" >&2
        exit 1
    fi
    echo "$value"
}

VERSION="$(versionPart MAJOR).$(versionPart MINOR).$(versionPart PATCH)"
APP_NAME="$(sed -n 's/^#define DISKSPACE_APP_NAME[[:space:]]\+"\(.*\)".*/\1/p' "$PRODUCT_H")"
: "${APP_NAME:=Disk Space}"

echo "  Version:  $VERSION"
echo "  Name:     $APP_NAME"
echo

# =============================================================================
# PREREQUISITES
# =============================================================================

for tool in cmake ninja pkg-config; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing $tool. Install it with:" >&2
        echo "  sudo apt install build-essential cmake ninja-build pkg-config libgtk-4-dev" >&2
        exit 1
    fi
done

if ! pkg-config --exists gtk4; then
    echo "GTK 4 development files not found. Install them with:" >&2
    echo "  sudo apt install libgtk-4-dev" >&2
    exit 1
fi

GTK_VERSION="$(pkg-config --modversion gtk4)"
echo "  GTK:      $GTK_VERSION"
echo

# =============================================================================
# BUILD
# =============================================================================

echo "=== Building (release) ==="
cmake -S "$LINUX_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR"
echo

# What the binary says it is, checked against what this script thinks it is
# building. They come from the same header, so a mismatch means a stale build
# directory rather than a typo - which is exactly the failure worth catching
# before it is packaged and uploaded.
REPORTED="$("$BUILD_DIR/diskscan" --version 2>/dev/null || true)"
if [ -n "$REPORTED" ] && [ "$REPORTED" != "$VERSION" ]; then
    echo "the binary reports $REPORTED but product.h says $VERSION - stale build?" >&2
    exit 1
fi

# =============================================================================
# TESTS
# =============================================================================
#
# Against the binary about to be packaged. A release that has not had its
# accuracy checked is a release nobody should trust, and the suite takes
# seconds.

if [ "$SKIP_TESTS" -eq 1 ]; then
    echo "=== Skipping the accuracy suite (--skip-tests) ==="
    echo
else
    echo "=== Accuracy suite ==="
    # Somewhere real. The suite refuses to run on a filesystem that misreports
    # sparse files, but pointing it at a good one is this script's job.
    export TMPDIR="${TMPDIR:-$HOME/.cache}/diskspace-export"
    mkdir -p "$TMPDIR"
    DISKSCAN="$BUILD_DIR/diskscan" "$LINUX_DIR/tests/accuracy.sh"
    echo
fi

# =============================================================================
# STAGING
# =============================================================================
#
# The AppDir layout appimagetool expects, which is also a perfectly ordinary
# tree: bin, share, and a desktop file at the top.

echo "=== Assembling the AppDir ==="

rm -rf "$RELEASE_DIR"
APPDIR="$RELEASE_DIR/$APP_ID.AppDir"
mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/share/applications"
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"

cp "$BUILD_DIR/diskspace" "$APPDIR/usr/bin/"
cp "$BUILD_DIR/diskscan" "$APPDIR/usr/bin/"
strip "$APPDIR/usr/bin/diskspace" "$APPDIR/usr/bin/diskscan" 2>/dev/null || true

DESKTOP="$LINUX_DIR/packaging/$APP_ID.desktop"
cp "$DESKTOP" "$APPDIR/usr/share/applications/"
cp "$DESKTOP" "$APPDIR/$APP_ID.desktop"

# The icon, from the shared assets the other two platforms draw theirs from.
ICON_SOURCE="$ROOT/assets/images/logo_300x300.png"
if [ -f "$ICON_SOURCE" ]; then
    cp "$ICON_SOURCE" "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_ID.png"
    cp "$ICON_SOURCE" "$APPDIR/$APP_ID.png"
else
    echo "  no icon at $ICON_SOURCE - the AppImage will use a default one"
fi

# AppRun is what an AppImage executes. A script rather than a symlink, so the
# bundled binary can find anything shipped beside it.
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export PATH="$HERE/usr/bin:$PATH"
export XDG_DATA_DIRS="$HERE/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$HERE/usr/bin/diskspace" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"
echo

# =============================================================================
# PACKAGING
# =============================================================================

echo "=== Packaging ==="

TARBALL="$RELEASE_DIR/DiskSpace-$VERSION-x86_64.tar.gz"
tar -czf "$TARBALL" -C "$RELEASE_DIR" "$(basename "$APPDIR")"
echo "  tarball:  $TARBALL"

APPIMAGE=""
if command -v appimagetool >/dev/null 2>&1; then
    APPIMAGE="$RELEASE_DIR/DiskSpace-$VERSION-x86_64.AppImage"
    # ARCH is not inferred reliably from an AppDir, and appimagetool fails
    # without it rather than guessing.
    ARCH=x86_64 appimagetool "$APPDIR" "$APPIMAGE" >/dev/null 2>&1
    echo "  AppImage: $APPIMAGE"
else
    echo "  AppImage: skipped, appimagetool is not on the path"
fi
echo

# =============================================================================
# CHECKSUMS
# =============================================================================
#
# Worth publishing for an unsigned build, where they are the only way anyone can
# check that what they downloaded is what was uploaded. Taken from the artefacts
# themselves rather than from a rebuild: the compiler does not produce identical
# bytes twice.

CHECKSUMS="$RELEASE_DIR/SHA256SUMS.txt"
(
    cd "$RELEASE_DIR"
    for artefact in *.tar.gz *.AppImage; do
        [ -e "$artefact" ] || continue
        sha256sum "$artefact"
    done
) > "$CHECKSUMS"

# =============================================================================
# DONE
# =============================================================================

echo "============================================"
echo "  Export complete!"
echo "  Version:   $VERSION"
echo "  Tarball:   $(du -h "$TARBALL" | cut -f1)"
[ -n "$APPIMAGE" ] && echo "  AppImage:  $(du -h "$APPIMAGE" | cut -f1)"
echo "  Checksums: $CHECKSUMS"
echo "============================================"
echo
sed 's/^/  /' "$CHECKSUMS"
echo
echo "Publish the checksums beside the download, so anyone who wants to can"
echo "verify what they got with:"
echo "  sha256sum -c SHA256SUMS.txt"
echo
echo "Both artefacts link GTK rather than bundling it, so they need GTK 4.10 or"
echo "later on the machine that runs them. Say so beside the download."
echo
if [ -z "$APPIMAGE" ]; then
    echo "For the single-file build, install appimagetool and run this again:"
    echo "  wget -O ~/.local/bin/appimagetool \\"
    echo "    https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
    echo "  chmod +x ~/.local/bin/appimagetool"
    echo "  sudo apt install libfuse2t64   # it is an AppImage itself, and needs FUSE"
    echo
fi
echo "Also remember to add the release to site/lib/releases.ts, or the in-app"
echo "update check will go on reporting that this version is the latest one."
