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
APP_VERSION="1.1.4"

# Must be an integer and must increase with every submission to Apple.
BUILD_NUMBER="8"

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

# Half the shipped binary was its symbol table: 12,801 symbols, 508 KB of
# __LINKEDIT against 508 KB of __TEXT. Swift does not strip a release build, so
# every internal symbol name travelled to users for nothing.
#
# -x removes local symbols and leaves everything the runtime resolves by name,
# which is the conservative choice; the more aggressive -rSTx saves another
# 56 KB and is not worth the question it raises. Before signing, because
# changing a byte of a signed binary invalidates the signature.
#
# The consequence to know about: a crash report from a stripped binary needs the
# unstripped copy in .build/release to symbolicate. Keep it with the release.
before_strip=$(stat -f%z "$APP_PATH/Contents/MacOS/DiskSpaceApp")
strip -x "$APP_PATH/Contents/MacOS/DiskSpaceApp"
after_strip=$(stat -f%z "$APP_PATH/Contents/MacOS/DiskSpaceApp")
echo "  stripped: $((before_strip / 1024)) KB -> $((after_strip / 1024)) KB"

# SwiftPM emits resources as a separate bundle beside the executable; it has to
# travel inside the app, where Assets looks for it in Contents/Resources.
for resource in .build/release/*.bundle; do
  [ -e "$resource" ] && cp -R "$resource" "$APP_PATH/Contents/Resources/"
done

# The images have to be in the app, and they have to be findable inside it.
#
# 1.1.1 shipped without this check and crashed on launch for everyone: the
# resource bundle was present, but the code asked SwiftPM's Bundle.module for
# it, and that looks beside the executable and then at an absolute path inside
# the machine that compiled the binary. On the build machine that second path
# existed, so the app ran here and nowhere else.
RESOURCE_BUNDLE="$APP_PATH/Contents/Resources/diskscan_DiskSpaceApp.bundle"
if [ ! -d "$RESOURCE_BUNDLE" ]; then
  echo "  ERROR: $RESOURCE_BUNDLE is missing — the app would launch without its"
  echo "         toolbar images. Did swift build emit the resource bundle?"
  exit 1
fi
missing_images=""
for image in collapse side_hide side_show zoom-in zoom-out; do
  [ -f "$RESOURCE_BUNDLE/$image.png" ] || missing_images="$missing_images $image.png"
done
if [ -n "$missing_images" ]; then
  echo "  ERROR: the resource bundle is missing:$missing_images"
  exit 1
fi
# Nothing outside the app may be needed to find them. A path into this machine's
# build directory baked into the binary is the bug above, ready to happen again.
if grep -q "$PACKAGE_DIR/.build" "$APP_PATH/Contents/MacOS/DiskSpaceApp" 2>/dev/null; then
  echo "  note: the binary still mentions this machine's build directory."
  echo "        Harmless while nothing reads Bundle.module — Assets finds the"
  echo "        bundle in Contents/Resources — but worth a look if resources"
  echo "        ever go missing on someone else's Mac."
fi

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

# =============================================================================
# CHECKSUMS
# =============================================================================
#
# The DMG is signed and notarized, so this is not how anyone establishes that it
# came from us — Gatekeeper does that, and it does it better than a hash
# published on the same site as the download. It is here so that someone who
# wants to check a download arrived intact, or to pin an exact build in a script
# or a package manifest, has the number to hand without downloading it twice.
#
# Written last: the resource-fork icon above does not change the data fork, but
# taking the hash after every step that touches the file removes the question.
# Taken from the artefact itself rather than from a rebuild, because the
# compiler does not produce identical bytes twice.
#
# Same format and same filename as the Linux and Windows exports, so all three
# verify the same way.

CHECKSUMS="$OUTPUT_DIR/SHA256SUMS.txt"
(
  cd "$OUTPUT_DIR"
  # The DMG only. The .app beside it is what went inside, and the notarize zip
  # is scaffolding — neither is a thing anyone downloads.
  shasum -a 256 "$(basename "$DMG_PATH")"
) > "$CHECKSUMS"

# =============================================================================
# HOMEBREW CASK
# =============================================================================
#
# A cask, not a formula: this is a GUI application in a disk image, which is
# what casks are for.
#
# The token is disk-space, not diskspace. That is what Homebrew's own naming
# rules make of "Disk Space", and diskspace is already taken in the main cask
# repository by an unrelated command line tool — a tap cannot outrank that, so
# the obvious token would have installed somebody else's software. It goes in a tap — a repository named homebrew-<something>
# holding Casks/disk-space.rb — and from then on the app installs and updates
# with brew like anything else.
#
# Written here rather than by hand because two of its fields are facts only this
# script knows: the version it just built and the checksum of the file it just
# produced. A cask with a stale sha256 fails to install, loudly, for everyone.

CASK_PATH="$OUTPUT_DIR/disk-space.rb"
DMG_SHA="$(shasum -a 256 "$DMG_PATH" | awk '{print $1}')"

# Where the DMG will be downloaded from. The release lives on a host this script
# does not upload to, so: whatever the environment says, else whatever the site
# already publishes for this version, else a placeholder that is obviously one.
DOWNLOAD_URL="${DISKSPACE_DOWNLOAD_URL:-}"
if [ -z "$DOWNLOAD_URL" ]; then
  # No match is the ordinary case for a version that has not been published yet,
  # and the placeholder below is what handles it. Without the `|| true` the failed
  # grep takes the whole script down under `set -e` and `pipefail`, which is what
  # it did on 1.1.2 — silently, after the DMG was already built and notarized.
  DOWNLOAD_URL="$(
    grep -A 4 "version: \"$APP_VERSION\"" "$REPO_ROOT/site/lib/releases.ts" 2>/dev/null |
      grep -m 1 -o 'url: "[^"]*DiskSpace-[^"]*\.dmg[^"]*"' |
      sed 's/^url: "//; s/"$//' || true
  )" || true
fi
if [ -z "$DOWNLOAD_URL" ]; then
  DOWNLOAD_URL="https://www.diskspace.io/download/DiskSpace-$APP_VERSION.dmg"
  CASK_URL_IS_A_GUESS=1
fi

cat > "$CASK_PATH" <<CASK
cask "disk-space" do
  version "$APP_VERSION"
  sha256 "$DMG_SHA"

  url "$DOWNLOAD_URL"
  name "Disk Space"
  desc "Disk usage analyser that shows where the space went while it scans"
  homepage "https://www.diskspace.io/"

  # The same endpoint the app itself asks when its About window opens, so brew
  # and the app agree about what the current version is.
  livecheck do
    url "https://www.diskspace.io/api/latest-version?platform=macos"
    strategy :json do |json|
      json.dig("latest", "version")
    end
  end

  # macOS 15, which is what the app requires. The bare symbol is the spelling
  # Homebrew's own linter asks for, and it means that version or later.
  depends_on macos: :sequoia

  app "$APP_NAME.app"

  # Everything the app writes outside itself: the per-folder scan dates that
  # "added since last scan" is measured against, the debug logs, and the
  # settings.
  zap trash: [
    "~/Library/Application Support/DiskSpace",
    "~/Library/Logs/DiskSpace",
    "~/Library/Preferences/$BUNDLE_ID.plist",
  ]
end
CASK

echo "✓ Homebrew cask written: $CASK_PATH"

# A tap checked out locally gets the file dropped straight in, so releasing is
# a commit in that repository rather than a copy-and-paste.
if [ -n "${DISKSPACE_TAP_DIR:-}" ] && [ -d "$DISKSPACE_TAP_DIR" ]; then
  mkdir -p "$DISKSPACE_TAP_DIR/Casks"
  cp "$CASK_PATH" "$DISKSPACE_TAP_DIR/Casks/disk-space.rb"
  echo "  copied into $DISKSPACE_TAP_DIR/Casks/disk-space.rb"
fi

# Ruby is what brew reads this with, so a syntax error is worth catching here
# rather than in somebody's terminal.
if command -v ruby >/dev/null 2>&1; then
  if ruby -c "$CASK_PATH" >/dev/null 2>&1; then
    echo "  syntax checked"
  else
    echo "  WARNING: the cask does not parse as Ruby"
    ruby -c "$CASK_PATH" 2>&1 | sed 's/^/    /'
  fi
fi

echo ""
echo "============================================"
echo "  Build complete!"
echo "  Version:   $APP_VERSION ($BUILD_NUMBER)"
echo "  App:       $APP_PATH"
echo "  DMG:       $DMG_PATH"
echo "  Checksums: $CHECKSUMS"
echo "============================================"
echo ""
sed 's/^/  /' "$CHECKSUMS"
echo ""
echo "Publish the checksums beside the download, so anyone who wants to can"
echo "verify what they got with:"
echo "  shasum -a 256 -c SHA256SUMS.txt"
echo ""
if [ -n "${CASK_URL_IS_A_GUESS:-}" ]; then
  echo "The cask's url is a placeholder: nothing here knew where this DMG will be"
  echo "published. Set DISKSPACE_DOWNLOAD_URL, or add the release to"
  echo "site/lib/releases.ts first, and run this again."
  echo ""
fi
echo "To publish the Homebrew cask, commit it to the tap repository:"
echo "  cp $CASK_PATH <tap>/Casks/disk-space.rb"
echo "Set DISKSPACE_TAP_DIR to have that copy happen here. Then:"
echo "  brew tap chofter/tap && brew install --cask disk-space"
echo ""
