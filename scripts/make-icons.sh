#!/bin/bash
# Builds the app and volume icons from assets/images/icon_1024.png.
#
# Each size is quantised with pngquant before iconutil packs it. The artwork is
# a smooth-gradient illustration, which PNG's filters handle badly: unquantised,
# the icon set came to 1.2 MB against an 881 KB program. Quantising costs
# nothing visible and removes about three quarters of that.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
SRC="assets/images/icon_1024.png"
OUT="diskspace-macos/Packaging"

[ -f "$SRC" ] || { echo "ERROR: $SRC not found"; exit 1; }

# Required, not optional: without it the icon set is roughly four times larger
# than the program it ships with, and the difference is not visible.
if ! command -v pngquant >/dev/null 2>&1; then
  echo "ERROR: pngquant is not installed."
  echo ""
  echo "  Icons are quantised before packing. Without it, AppIcon.icns is about"
  echo "  1.2 MB instead of 355 KB — larger than the app binary — for no visible"
  echo "  difference in the artwork."
  echo ""
  echo "  Install it with:"
  echo "    brew install pngquant"
  echo ""
  echo "  (Homebrew: https://brew.sh)"
  exit 1
fi

# $1 = output icns, remaining args = "pixels name" pairs for the icon set.
build_icns() {
  local target="$1"; shift
  local set_dir
  set_dir="$(mktemp -d)/icon.iconset"
  mkdir -p "$set_dir"

  for spec in "$@"; do
    set -- $spec
    sips -z "$1" "$1" "$SRC" --out "$set_dir/icon_$2.png" >/dev/null 2>&1
    # --skip-if-larger keeps the original when quantising would not help, which
    # happens at the smallest sizes.
    pngquant --quality=80-95 --speed 1 --force --skip-if-larger \
      --output "$set_dir/icon_$2.png" "$set_dir/icon_$2.png" 2>/dev/null || true
  done

  # Packed directly rather than through iconutil, which re-encodes and would
  # discard the quantisation.
  python3 "$(dirname "${BASH_SOURCE[0]}")/pack-icns.py" "$set_dir" "$target"
  rm -rf "$(dirname "$set_dir")"
}

echo "=== Building icons ==="

# The app icon carries every size: the 1024px slot is what Get Info and the
# largest Finder icon view use on a Retina display.
build_icns "$OUT/AppIcon.icns" \
  "16 16x16" "32 16x16@2x" "32 32x32" "64 32x32@2x" \
  "128 128x128" "256 128x128@2x" "256 256x256" "512 256x256@2x" \
  "512 512x512" "1024 512x512@2x"

# The volume and .dmg file icons stop at 512px: nothing draws them larger, and
# this set is embedded twice in the disk image.
build_icns "$OUT/VolumeIcon.icns" \
  "16 16x16" "32 16x16@2x" "32 32x32" "64 32x32@2x" \
  "128 128x128" "256 128x128@2x" "256 256x256" "512 256x256@2x"

echo "✓ Icons written to $OUT"
