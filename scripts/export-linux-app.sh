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
# Neither bundles anything, and neither needs to. The application links libX11,
# libXft, libXrender and libfontconfig, all four of which are what an X desktop
# is built on - Xfce and GTK 3 both depend on every one - so this is as close to
# the portable Windows .exe as Linux gets: one file, nothing to install.
#
# Prerequisites:
#   - build-essential, cmake, ninja-build, pkg-config
#   - libx11-dev, libxft-dev, libxrender-dev, libfontconfig-dev
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
        echo "  sudo apt install build-essential cmake ninja-build pkg-config \\" >&2
        echo "      libx11-dev libxft-dev libxrender-dev libfontconfig-dev" >&2
        exit 1
    fi
done

for library in x11 xft xrender fontconfig; do
    if ! pkg-config --exists "$library"; then
        echo "development files for $library not found. Install them with:" >&2
        echo "  sudo apt install libx11-dev libxft-dev libxrender-dev libfontconfig-dev" >&2
        exit 1
    fi
done

echo "  X11:      $(pkg-config --modversion x11)"
echo "  Xft:      $(pkg-config --modversion xft)"
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

# Permissions set explicitly rather than inherited.
#
# `cp` carries the source file's mode across, and what that mode is depends on
# where the build happened. Building on a drvfs mount under WSL - which is how
# this is developed - reports every file as 0777, so the tarball would record
# the icon and the desktop file as executable and nobody would notice until it
# was unpacked somewhere that cared.
#
# Stating it here means the artefacts carry the same modes whatever built them,
# and the person who downloads one does not have to chmod anything.
setPermissions() {
    local dir="$1"
    chmod 755 "$dir/AppRun" "$dir/usr/bin/diskspace" "$dir/usr/bin/diskscan"
    chmod 644 "$dir/$APP_ID.desktop" "$dir/usr/share/applications/$APP_ID.desktop"
    [ -f "$dir/$APP_ID.png" ] && chmod 644 "$dir/$APP_ID.png"
    [ -f "$dir/usr/share/icons/hicolor/256x256/apps/$APP_ID.png" ] &&
        chmod 644 "$dir/usr/share/icons/hicolor/256x256/apps/$APP_ID.png"
    find "$dir" -type d -exec chmod 755 {} +
    return 0
}
setPermissions "$APPDIR"

# Whether that actually did anything.
#
# Some filesystems ignore chmod and report a fixed mode for everything. The one
# that matters here is drvfs, the Windows drive under WSL, which answers 0777 to
# every question - so a build done on /mnt/c produces a tarball recording the
# icon and the desktop file as executable, and nothing says so until somebody
# unpacks it on a machine that cares.
#
# Rather than leave that to chance, the tarball is built from a copy staged
# somewhere modes survive. Checked rather than assumed, so a normal build on a
# normal filesystem does no extra work.
TAR_SOURCE_DIR="$RELEASE_DIR"
if [ "$(stat -c %a "$APPDIR/$APP_ID.desktop")" != "644" ]; then
    STAGE="${TMPDIR:-/tmp}/diskspace-export-stage"
    echo "  $RELEASE_DIR ignores chmod; staging via $STAGE for the tarball"
    rm -rf "$STAGE"
    mkdir -p "$STAGE"
    cp -r "$APPDIR" "$STAGE/"
    setPermissions "$STAGE/$(basename "$APPDIR")"
    if [ "$(stat -c %a "$STAGE/$(basename "$APPDIR")/$APP_ID.desktop")" != "644" ]; then
        echo "permissions do not stick in $STAGE either - set TMPDIR to a real filesystem" >&2
        exit 1
    fi
    TAR_SOURCE_DIR="$STAGE"
fi
echo

# =============================================================================
# PACKAGING
# =============================================================================

echo "=== Packaging ==="

TARBALL="$RELEASE_DIR/DiskSpace-$VERSION-x86_64.tar.gz"
# --owner and --group so the archive does not carry whoever happened to build it,
# and unpacks to the person unpacking it rather than to a uid they have never
# heard of.
tar -czf "$TARBALL" --owner=0 --group=0 -C "$TAR_SOURCE_DIR" "$(basename "$APPDIR")"
chmod 644 "$TARBALL"
echo "  tarball:  $TARBALL"

APPIMAGE=""
if command -v appimagetool >/dev/null 2>&1; then
    APPIMAGE="$RELEASE_DIR/DiskSpace-$VERSION-x86_64.AppImage"
    # From the same tree the tarball came from, so the squashfs inside the
    # AppImage records the same modes rather than whatever the build filesystem
    # claimed.
    # ARCH is not inferred reliably from an AppDir, and appimagetool fails
    # without it rather than guessing.
    ARCH=x86_64 appimagetool "$TAR_SOURCE_DIR/$(basename "$APPDIR")" "$APPIMAGE" >/dev/null 2>&1
    # An AppImage that is not executable is a file nobody can run, and the error
    # a desktop gives for it says nothing useful. appimagetool usually sets this
    # itself; saying so here costs nothing and removes the usual first thing that
    # goes wrong with a downloaded build.
    chmod 755 "$APPIMAGE"
    echo "  AppImage: $APPIMAGE"
else
    echo "  AppImage: skipped, appimagetool is not on the path"
fi
echo

# =============================================================================
# NATIVE PACKAGES (.deb / .rpm)
# =============================================================================
#
# The installable route, and the one to lead with. A .deb or .rpm declares the
# handful of libraries the binary links, so the package manager pulls in
# anything missing, drops a menu entry, and needs no chmod - none of which the
# AppImage can claim on a machine without libfuse2, which is every stock Ubuntu
# since 22.04.
#
# Built from the same mode-correct tree the tarball came from, reshaped into a
# plain /usr layout: no AppRun, no top-level icon, just what belongs under a
# system prefix. Each packager is optional and guarded like appimagetool above,
# because neither tool ships in a default Debian install (dpkg-deb does; rpmbuild
# is one `apt install rpm` away) and a Fedora box is not required to produce an
# rpm.

echo "=== Native packages ==="

SRC_APPDIR="$TAR_SOURCE_DIR/$(basename "$APPDIR")"
PKG_STAGE="$TAR_SOURCE_DIR/pkg"
PKGROOT="$PKG_STAGE/root"
METAINFO="$LINUX_DIR/packaging/$APP_ID.metainfo.xml"

# Package identity. The name is the cask/command name, lowercase, so the three
# platforms agree on what the thing is called. The maintainer is the publisher,
# not a person: override it with DISKSPACE_MAINTAINER if a real contact is wanted.
PKG_NAME="disk-space"
MAINTAINER="${DISKSPACE_MAINTAINER:-Chofter Apps <noreply@chofter.com>}"
HOMEPAGE="https://www.diskspace.io"
SHORT_DESC="Disk usage analyzer"
LONG_DESC="Find what is filling your disk, and what arrived recently."

rm -rf "$PKG_STAGE"
mkdir -p "$PKGROOT/usr"
# -a to carry the modes setPermissions already fixed on the source tree.
cp -a "$SRC_APPDIR/usr/." "$PKGROOT/usr/"
# AppStream metadata, so a software centre has something to show. Harmless where
# nothing reads it.
if [ -f "$METAINFO" ]; then
    install -D -m644 "$METAINFO" "$PKGROOT/usr/share/metainfo/$APP_ID.metainfo.xml"
fi

DEB=""
RPM=""

# --- .deb -------------------------------------------------------------------
#
# dpkg-deb does not read the binary's NEEDED entries the way dpkg-shlibdeps
# would, so the dependency line is stated here. No version floor: these four are
# on every X desktop, and the script's closing note explains why.
if command -v dpkg-deb >/dev/null 2>&1; then
    DEBROOT="$PKG_STAGE/deb"
    mkdir -p "$DEBROOT"
    cp -a "$PKGROOT/." "$DEBROOT/"
    mkdir -p "$DEBROOT/DEBIAN"
    INSTALLED_KB="$(du -sk "$DEBROOT/usr" | cut -f1)"
    cat > "$DEBROOT/DEBIAN/control" <<CONTROL
Package: $PKG_NAME
Version: $VERSION
Architecture: amd64
Maintainer: $MAINTAINER
Installed-Size: $INSTALLED_KB
Depends: libx11-6, libxft2, libxrender1, libfontconfig1, libpng16-16
Section: utils
Priority: optional
Homepage: $HOMEPAGE
Description: $SHORT_DESC
 $LONG_DESC
CONTROL
    chmod 644 "$DEBROOT/DEBIAN/control"
    DEB="$RELEASE_DIR/DiskSpace-$VERSION-amd64.deb"
    # --root-owner-group so the archive records root:root without fakeroot.
    dpkg-deb --root-owner-group --build "$DEBROOT" "$DEB" >/dev/null
    chmod 644 "$DEB"
    echo "  deb:      $DEB"
else
    echo "  deb:      skipped, dpkg-deb is not on the path (it ships with dpkg)"
fi

# --- .rpm -------------------------------------------------------------------
#
# rpmbuild does read the ELF, so its Requires are generated from the binary and
# there is no list here to drift. Only files are listed in %files, never the
# shared /usr directories, which belong to the filesystem package.
if command -v rpmbuild >/dev/null 2>&1; then
    RPMTOP="$PKG_STAGE/rpmbuild"
    mkdir -p "$RPMTOP/BUILD" "$RPMTOP/RPMS" "$RPMTOP/SPECS"
    SPEC="$RPMTOP/SPECS/$PKG_NAME.spec"
    {
        # No separate debuginfo package, and no /usr/lib/.build-id links: this
        # ships stripped release binaries, and the build-id links would otherwise
        # land in the buildroot unpackaged and fail the build on some rpmbuilds.
        echo "%global debug_package %{nil}"
        echo "%global _build_id_links none"
        echo ""
        echo "Name:      $PKG_NAME"
        echo "Version:   $VERSION"
        echo "Release:   1"
        echo "Summary:   $SHORT_DESC"
        echo "License:   Proprietary"
        echo "URL:       $HOMEPAGE"
        echo "BuildArch: x86_64"
        echo ""
        echo "%description"
        echo "$LONG_DESC"
        echo ""
        echo "%install"
        echo "cp -a $PKGROOT/. %{buildroot}/"
        echo ""
        echo "%files"
        echo "/usr/bin/diskspace"
        echo "/usr/bin/diskscan"
        echo "/usr/share/applications/$APP_ID.desktop"
        echo "/usr/share/icons/hicolor/256x256/apps/$APP_ID.png"
        [ -f "$PKGROOT/usr/share/metainfo/$APP_ID.metainfo.xml" ] &&
            echo "/usr/share/metainfo/$APP_ID.metainfo.xml"
    } > "$SPEC"
    if rpmbuild --define "_topdir $RPMTOP" -bb "$SPEC" >/dev/null 2>&1; then
        BUILT_RPM="$(find "$RPMTOP/RPMS" -name '*.rpm' | head -1)"
        if [ -n "$BUILT_RPM" ]; then
            RPM="$RELEASE_DIR/DiskSpace-$VERSION-x86_64.rpm"
            cp "$BUILT_RPM" "$RPM"
            chmod 644 "$RPM"
            echo "  rpm:      $RPM"
        else
            echo "  rpm:      rpmbuild succeeded but produced no package" >&2
        fi
    else
        echo "  rpm:      rpmbuild failed; rerun it without redirection to see why" >&2
    fi
else
    echo "  rpm:      skipped, rpmbuild is not on the path (Debian/Ubuntu: sudo apt install rpm)"
fi

# Build intermediates, not artefacts. Removed so the release directory holds
# only the packages and their checksums.
rm -rf "$PKG_STAGE"
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
    for artefact in *.tar.gz *.AppImage *.deb *.rpm; do
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
[ -n "$DEB" ] && echo "  deb:       $(du -h "$DEB" | cut -f1)"
[ -n "$RPM" ] && echo "  rpm:       $(du -h "$RPM" | cut -f1)"
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
echo "Everything here needs libX11, libXft, libXrender and libfontconfig on the"
echo "machine that runs it - which any X desktop already has, since Xfce and"
echo "GTK 3 depend on all four. There is no version floor worth publishing."
echo
echo "The .deb and .rpm are the installable route, and the one to put first: the"
echo "package manager resolves those four libraries, so nothing has to be chmod'd"
echo "and the app lands in the menu."
if [ -n "$DEB" ]; then
    echo "  sudo apt install ./$(basename "$DEB")      # Debian, Ubuntu"
fi
if [ -n "$RPM" ]; then
    echo "  sudo dnf install ./$(basename "$RPM")   # Fedora, and zypper on openSUSE"
fi
echo
echo "The AppImage is already executable, and so are the binaries in the tarball."
echo "Two things outside a build's control undo that: a browser download, and a"
echo "FAT-formatted USB stick, neither of which can carry the bit. Where a build"
echo "arrives by one of those routes it still needs chmod +x."
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
