#!/bin/bash
# =============================================================================
# export-macos-app.sh
#
# Builds, signs, notarizes and packages a distributable release of Disk Space.
#
# Usage:
#   ./scripts/export-macos-app.sh
#
# Prerequisites (see "Manual setup" at the bottom of this file):
#   - Xcode command line tools
#   - A "Developer ID Application" certificate in your Keychain
#   - Notarization credentials stored in your Keychain
# =============================================================================

set -euo pipefail

cleanup() {
  rm -rf "${OUTPUT_DIR:-}/dmg-staging" 2>/dev/null || true
  rm -f "${NOTARIZE_ZIP:-}" 2>/dev/null || true
  rm -f "${TEMP_DMG:-}" 2>/dev/null || true
}
trap cleanup EXIT

# =============================================================================
# CONFIGURATION
# =============================================================================

# Apple Developer Team ID. Find it with:
#   security find-identity -v -p codesigning
# It appears in parentheses after the certificate name.
TEAM_ID="${DISKSPACE_TEAM_ID:-TEAMID}"

# Apple ID used for the developer account.
APPLE_ID="${DISKSPACE_APPLE_ID:-shaneosullivan1@gmail.com}"

# Keychain profile holding the notarization credentials. Created once with:
#   xcrun notarytool store-credentials "DiskSpaceNotary" \
#     --apple-id YOUR_APPLE_ID --team-id YOUR_TEAM_ID \
#     --password YOUR_APP_SPECIFIC_PASSWORD
NOTARY_KEYCHAIN_PROFILE="${DISKSPACE_NOTARY_PROFILE:-DiskSpaceNotary}"

# Only needed on the very first run; the script stores it and you can clear it.
NOTARY_PASSWORD="${DISKSPACE_NOTARY_PASSWORD:-}"

# Shown in Finder and the About window. Increment for each release.
APP_VERSION="1.0.4"

# Must be an integer and must increase with every submission to Apple.
BUILD_NUMBER="4"

APP_NAME="Disk Space"
BUNDLE_ID="com.chofter.diskspace"

# =============================================================================
# PATHS
# =============================================================================

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE_DIR="$REPO_ROOT/diskspace-macos"
OUTPUT_DIR="$REPO_ROOT/build/release"
APP_PATH="$OUTPUT_DIR/$APP_NAME.app"
DMG_PATH="$OUTPUT_DIR/DiskSpace-$APP_VERSION.dmg"
NOTARIZE_ZIP="$OUTPUT_DIR/DiskSpace-notarize.zip"

echo "=== Disk Space — macOS Export ==="
echo ""

# =============================================================================
# VALIDATION
# =============================================================================

if [ -z "$APPLE_ID" ] || [ -z "$TEAM_ID" ]; then
  echo "ERROR: APPLE_ID and TEAM_ID must be set at the top of this script."
  exit 1
fi

SIGN_IDENTITY="$(security find-identity -v -p codesigning \
  | grep "Developer ID Application" | grep "$TEAM_ID" | head -1 \
  | sed -E 's/.*"(.*)"/\1/')"

if [ -z "$SIGN_IDENTITY" ]; then
  echo "ERROR: No 'Developer ID Application' certificate found for team $TEAM_ID."
  echo ""
  echo "To create one:"
  echo "  1. Xcode → Settings → Accounts → select your team"
  echo "  2. Manage Certificates → '+' → 'Developer ID Application'"
  echo "  Or: https://developer.apple.com/account/resources/certificates"
  exit 1
fi

echo "✓ Signing identity: $SIGN_IDENTITY"

# =============================================================================
# STORE NOTARIZATION CREDENTIALS (first run only)
# =============================================================================

if [ -n "$NOTARY_PASSWORD" ]; then
  echo ""
  echo "=== Storing notarization credentials in Keychain ==="
  xcrun notarytool store-credentials "$NOTARY_KEYCHAIN_PROFILE" \
    --apple-id "$APPLE_ID" \
    --team-id "$TEAM_ID" \
    --password "$NOTARY_PASSWORD"
  echo "✓ Credentials stored — you can clear NOTARY_PASSWORD from this script."
fi

if ! xcrun notarytool history --keychain-profile "$NOTARY_KEYCHAIN_PROFILE" >/dev/null 2>&1; then
  echo "ERROR: No notarization credentials found under profile"
  echo "       '$NOTARY_KEYCHAIN_PROFILE'."
  echo ""
  echo "Store them once with:"
  echo "  xcrun notarytool store-credentials \"$NOTARY_KEYCHAIN_PROFILE\" \\"
  echo "    --apple-id \"$APPLE_ID\" --team-id \"$TEAM_ID\" \\"
  echo "    --password YOUR_APP_SPECIFIC_PASSWORD"
  exit 1
fi

echo "✓ Notarization credentials found"

# =============================================================================
# BUILD
# =============================================================================

echo ""
echo "=== Building (release) ==="
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

cd "$PACKAGE_DIR"
swift build -c release
echo "✓ Built"

# =============================================================================
# ASSEMBLE THE BUNDLE
# =============================================================================
#
# There is no Xcode project, so the bundle is assembled here rather than by
# xcodebuild. Everything must be in place before signing: adding a file to a
# signed bundle invalidates the signature and notarization will reject it.

echo ""
echo "=== Assembling $APP_NAME.app ==="
mkdir -p "$APP_PATH/Contents/MacOS" "$APP_PATH/Contents/Resources"

cat > "$APP_PATH/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key>
	<string>$APP_NAME</string>
	<key>CFBundleDisplayName</key>
	<string>$APP_NAME</string>
	<key>CFBundleIdentifier</key>
	<string>$BUNDLE_ID</string>
	<key>CFBundleExecutable</key>
	<string>DiskSpaceApp</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$APP_VERSION</string>
	<key>CFBundleVersion</key>
	<string>$BUILD_NUMBER</string>
	<key>LSMinimumSystemVersion</key>
	<string>15.0</string>
	<key>LSApplicationCategoryType</key>
	<string>public.app-category.utilities</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSHumanReadableCopyright</key>
	<string>Copyright © $(date +%Y) Shane O'Sullivan. All rights reserved.</string>
</dict>
</plist>
PLIST

# Optional app icon. Drop an AppIcon.icns into diskspace-macos/Packaging to use
# one; without it the app shows the generic application icon.
if [ -f "$PACKAGE_DIR/Packaging/AppIcon.icns" ]; then
  cp "$PACKAGE_DIR/Packaging/AppIcon.icns" "$APP_PATH/Contents/Resources/"
  /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string AppIcon" \
    "$APP_PATH/Contents/Info.plist"
  echo "✓ App icon included"
else
  echo "  note: no Packaging/AppIcon.icns — shipping without a custom icon"
fi

cp .build/release/DiskSpaceApp "$APP_PATH/Contents/MacOS/DiskSpaceApp"

# SwiftPM emits resources as a separate bundle beside the executable; it has to
# travel inside the app or Bundle.module finds nothing at runtime.
for resource in .build/release/*.bundle; do
  [ -e "$resource" ] && cp -R "$resource" "$APP_PATH/Contents/Resources/"
done

echo "✓ Bundle assembled: $APP_PATH"

# =============================================================================
# SIGN
# =============================================================================
#
# Nested code is signed first and the outer bundle last: signing the outer
# bundle seals what is inside it, so anything signed afterwards breaks the seal.
#
# --options runtime enables the Hardened Runtime, which notarization requires.
# --timestamp records a secure timestamp so the signature outlives the
# certificate's expiry.

echo ""
echo "=== Signing ==="

for nested in "$APP_PATH/Contents/Resources"/*.bundle; do
  [ -e "$nested" ] || continue
  # A SwiftPM resource bundle is a plain directory with no Info.plist, which
  # codesign will not accept as a bundle. It holds no code, so the app's own
  # signature seals it and nothing further is needed. Anything that *is* a real
  # bundle must still be signed before the app that contains it.
  if [ -f "$nested/Contents/Info.plist" ] || [ -f "$nested/Info.plist" ]; then
    codesign --force --options runtime --timestamp \
      --sign "$SIGN_IDENTITY" "$nested"
    echo "  signed $(basename "$nested")"
  else
    echo "  $(basename "$nested"): resources only, sealed by the app signature"
  fi
done

codesign --force --options runtime --timestamp \
  --sign "$SIGN_IDENTITY" "$APP_PATH"

codesign --verify --deep --strict --verbose=2 "$APP_PATH"
echo "✓ Signature valid"

# =============================================================================
# NOTARIZE THE APP
# =============================================================================

echo ""
echo "=== Notarizing app (several minutes) ==="
ditto -c -k --keepParent "$APP_PATH" "$NOTARIZE_ZIP"

xcrun notarytool submit "$NOTARIZE_ZIP" \
  --keychain-profile "$NOTARY_KEYCHAIN_PROFILE" \
  --wait

echo "✓ Notarized"

echo ""
echo "=== Stapling ticket ==="
xcrun stapler staple "$APP_PATH"
spctl --assess --type execute --verbose "$APP_PATH"
echo "✓ Gatekeeper assessment passed"

# =============================================================================
# CREATE DMG
# =============================================================================

echo ""
echo "=== Creating DMG ==="
STAGING_DIR="$OUTPUT_DIR/dmg-staging"
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"
cp -R "$APP_PATH" "$STAGING_DIR/"
ln -s /Applications "$STAGING_DIR/Applications"

ICNS="$PACKAGE_DIR/Packaging/AppIcon.icns"
# Trimmed to 512px: a volume icon and a file icon are never drawn larger, and
# this one is embedded twice, so the full set would cost 1.6 MB for no gain.
VOLUME_ICNS="$PACKAGE_DIR/Packaging/VolumeIcon.icns"
[ -f "$VOLUME_ICNS" ] || VOLUME_ICNS="$ICNS"
BACKGROUND="$PACKAGE_DIR/Packaging/dmg-background.png"
if [ -f "$BACKGROUND" ]; then
  mkdir -p "$STAGING_DIR/.background"
  cp "$BACKGROUND" "$STAGING_DIR/.background/background.png"
fi

# Read-write first: the window layout, background and volume icon can only be
# applied to a mounted volume. Compressed at the end.
TEMP_DMG="$OUTPUT_DIR/DiskSpace-rw.dmg"
rm -f "$TEMP_DMG"

hdiutil create \
  -volname "$APP_NAME" \
  -srcfolder "$STAGING_DIR" \
  -ov -format UDRW \
  "$TEMP_DMG"

MOUNT_OUTPUT="$(hdiutil attach -readwrite -noverify -noautoopen "$TEMP_DMG")"
VOLUME_PATH="$(echo "$MOUNT_OUTPUT" | grep -o '/Volumes/[^\t]*' | head -1 | sed 's/[[:space:]]*$//')"
if [ -z "$VOLUME_PATH" ]; then
  echo "ERROR: mounted the image but found no /Volumes path in hdiutil's output."
  exit 1
fi
VOLUME_NAME="$(basename "$VOLUME_PATH")"
APP_BASENAME="$(basename "$APP_PATH")"
echo "  mounted at $VOLUME_PATH"

if [ -f "$VOLUME_ICNS" ]; then
  cp "$VOLUME_ICNS" "$VOLUME_PATH/.VolumeIcon.icns"
  SetFile -a C "$VOLUME_PATH"
  echo "✓ Volume icon applied"
fi

# Lay out the installer window.
#
# The icons are positioned to match the background: the app on the left, the
# Applications symlink on the right, with the drawn arrow between them.
#
# `.VolumeIcon.icns` and `.background` are parked far outside the window bounds
# as well as being flagged invisible. The invisible flag alone is not enough —
# Finder reveals such files when "show hidden files" is turned on — but the
# window has a fixed size and no scrollbars, so anything beyond it stays out of
# sight either way.
echo ""
echo "=== Laying out the installer window ==="

# Driving Finder needs Automation permission, which is granted per calling app.
# The first run prompts; if it is denied, or the script runs somewhere that
# cannot prompt, osascript fails with -1743 and the DMG silently ships with the
# default Finder layout. Fail loudly instead.
set +e
LAYOUT_ERROR="$(osascript <<APPLESCRIPT
tell application "Finder"
  tell disk "$VOLUME_NAME"
    open
    set current view of container window to icon view
    set toolbar visible of container window to false
    set statusbar visible of container window to false
    set the bounds of container window to {200, 200, 800, 520}
    set viewOptions to the icon view options of container window
    set arrangement of viewOptions to not arranged
    set icon size of viewOptions to 128
    set text size of viewOptions to 12
    try
      set background picture of viewOptions to file ".background:background.png"
    end try
    close
    open
    delay 2
    set position of item "$APP_BASENAME" of container window to {150, 170}
    set position of item "Applications" of container window to {450, 170}
    try
      set position of item ".VolumeIcon.icns" of container window to {950, 950}
    end try
    try
      set position of item ".background" of container window to {950, 1050}
    end try
    update without registering applications
    delay 1
    close
  end tell
end tell
APPLESCRIPT
2>&1 >/dev/null)"
LAYOUT_STATUS=$?
set -e

if [ $LAYOUT_STATUS -ne 0 ]; then
  echo ""
  echo "ERROR: could not lay out the installer window."
  echo "  $LAYOUT_ERROR"
  echo ""
  if echo "$LAYOUT_ERROR" | grep -q '1743'; then
    echo "  This is an Automation permission, not a scripting fault. Grant it in:"
    echo "    System Settings → Privacy & Security → Automation"
    echo "    → <the app running this script, e.g. Terminal> → enable Finder"
    echo ""
    echo "  Running the script again from a terminal will prompt for it the"
    echo "  first time; approve the prompt and re-run."
  fi
  hdiutil detach "$VOLUME_PATH" -force -quiet 2>/dev/null || true
  exit 1
fi
echo "✓ Window laid out"

# Flag the helpers invisible, and drop the metadata Finder just created.
chflags hidden "$VOLUME_PATH/.VolumeIcon.icns" 2>/dev/null || true
chflags hidden "$VOLUME_PATH/.background" 2>/dev/null || true
rm -rf "$VOLUME_PATH/.fseventsd" "$VOLUME_PATH/.Trashes" 2>/dev/null || true

sync
hdiutil detach "$VOLUME_PATH" -quiet \
  || { sleep 2; hdiutil detach "$VOLUME_PATH" -force -quiet; }

# Compress. Done last so the shipped image carries the layout.
rm -f "$DMG_PATH"
hdiutil convert "$TEMP_DMG" \
  -format UDZO -imagekey zlib-level=9 \
  -o "$DMG_PATH"

rm -f "$TEMP_DMG"
rm -rf "$STAGING_DIR"
echo "✓ DMG created: $DMG_PATH"

# =============================================================================
# NOTARIZE THE DMG
# =============================================================================
#
# The DMG is notarized separately from the app it contains: Gatekeeper checks
# the disk image itself when a user opens a download.

echo ""
echo "=== Notarizing DMG ==="
xcrun notarytool submit "$DMG_PATH" \
  --keychain-profile "$NOTARY_KEYCHAIN_PROFILE" \
  --wait
xcrun stapler staple "$DMG_PATH"
echo "✓ DMG notarized and stapled"

# =============================================================================
# ICON ON THE DMG FILE ITSELF
# =============================================================================
#
# Distinct from the volume icon set earlier. That one is a file *inside* the
# image and is what Finder shows once the image is mounted. This one is the icon
# of the .dmg file as it sits in a folder.
#
# It lives in the file's resource fork rather than in its contents, so it is
# applied last: it does not touch the data fork and leaves the staple intact.
#
# Caveat: extended attributes are not carried over HTTP, so anyone downloading
# the DMG sees the generic disk-image icon regardless. This only shows for copies
# that stay on a Mac filesystem — over AirDrop, a shared volume, or locally.

if [ -f "$VOLUME_ICNS" ] && command -v Rez >/dev/null 2>&1; then
  echo ""
  echo "=== Setting the DMG file's own icon ==="
  ICON_TMP="$(mktemp -d)"
  cp "$VOLUME_ICNS" "$ICON_TMP/icon.icns"
  sips -i "$ICON_TMP/icon.icns" >/dev/null 2>&1
  DeRez -only icns "$ICON_TMP/icon.icns" > "$ICON_TMP/icon.rsrc" 2>/dev/null
  Rez -append "$ICON_TMP/icon.rsrc" -o "$DMG_PATH"
  SetFile -a C "$DMG_PATH"
  rm -rf "$ICON_TMP"
  # Re-check: appending a resource fork should not disturb the ticket.
  xcrun stapler validate "$DMG_PATH" >/dev/null 2>&1 \
    && echo "✓ DMG icon set, staple still valid" \
    || echo "WARNING: staple no longer validates after setting the icon"
fi

echo ""
echo "============================================"
echo "  Build complete!"
echo "  Version: $APP_VERSION ($BUILD_NUMBER)"
echo "  App:     $APP_PATH"
echo "  DMG:     $DMG_PATH"
echo "============================================"
