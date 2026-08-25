#!/bin/bash
# Builds "Disk Space.app".
#
# A bundle is not optional for a Mac app: unbundled, the menu bar takes its name
# from the executable, and permissions such as Full Disk Access attach to the
# terminal that launched it rather than to the app itself.
set -euo pipefail
cd "$(dirname "$0")/.."

swift build -c release
APP="$(pwd)/build/Disk Space.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp Packaging/Info.plist "$APP/Contents/Info.plist"
[ -f Packaging/AppIcon.icns ] && cp Packaging/AppIcon.icns "$APP/Contents/Resources/"
cp .build/release/DiskSpaceApp "$APP/Contents/MacOS/DiskSpaceApp"
# SwiftPM emits resources as a separate bundle beside the executable; it has to
# travel with the app or Bundle.module finds nothing at runtime.
for resource in .build/release/*.bundle; do
  [ -e "$resource" ] && cp -R "$resource" "$APP/Contents/Resources/"
done
# Ad-hoc signature, so macOS keeps permissions attached across rebuilds.
codesign --force --sign - "$APP" 2>/dev/null || true
echo "$APP"
