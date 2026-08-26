#!/usr/bin/env bash
# Builds an unsigned .app for development.
#
# The menu bar takes the application's name from CFBundleName, which only exists
# inside a bundle. Running .build/release/DiskSpaceApp directly therefore shows
# "DiskSpaceApp" next to the Apple menu, and no runtime setting changes that.
# This assembles the same bundle the release script does, minus signing and
# notarisation, so development runs look like the shipped app.
#
# For a signed, notarised, distributable build use export-macos-app.sh instead.
set -euo pipefail

APP_NAME="Disk Space"
BUNDLE_ID="com.chofter.diskspace"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE_DIR="$ROOT/diskspace-macos"
OUTPUT_DIR="$ROOT/build/dev"
APP_PATH="$OUTPUT_DIR/$APP_NAME.app"

cd "$PACKAGE_DIR"
swift build -c release
BUILD_DIR="$(swift build -c release --show-bin-path)"

rm -rf "$APP_PATH"
mkdir -p "$APP_PATH/Contents/MacOS" "$APP_PATH/Contents/Resources"
cp "$BUILD_DIR/DiskSpaceApp" "$APP_PATH/Contents/MacOS/"
# SwiftPM keeps resources in their own bundles beside the binary; without them
# the app launches but every icon is missing.
for bundle in "$BUILD_DIR"/*.bundle; do
  [ -e "$bundle" ] && cp -R "$bundle" "$APP_PATH/Contents/Resources/"
done
[ -f "$PACKAGE_DIR/Packaging/AppIcon.icns" ] &&
  cp "$PACKAGE_DIR/Packaging/AppIcon.icns" "$APP_PATH/Contents/Resources/"

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
	<string>$BUNDLE_ID.dev</string>
	<key>CFBundleExecutable</key>
	<string>DiskSpaceApp</string>
	<key>CFBundleIconFile</key>
	<string>AppIcon</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>0.0.0</string>
	<key>CFBundleVersion</key>
	<string>0</string>
	<key>LSMinimumSystemVersion</key>
	<string>15.0</string>
	<key>NSHighResolutionCapable</key>
	<true/>
</dict>
</plist>
PLIST

echo "Built $APP_PATH"
