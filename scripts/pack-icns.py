#!/usr/bin/env python3
"""Packs an .iconset directory into an .icns, preserving each PNG byte for byte.

`iconutil` re-encodes whatever it is given, which throws away the palette that
pngquant produced — a 130 KB quantised image comes back out at 418 KB. The ICNS
container is trivial (magic, total length, then typed entries), and PNG-backed
entries have been supported since 10.7, so writing it directly keeps the
compression we asked for.
"""
import struct
import sys
from pathlib import Path

# Icon set filenames to their ICNS entry types.
TYPES = {
    "icon_16x16.png": b"icp4",
    "icon_16x16@2x.png": b"ic11",
    "icon_32x32.png": b"icp5",
    "icon_32x32@2x.png": b"ic12",
    "icon_128x128.png": b"ic07",
    "icon_128x128@2x.png": b"ic13",
    "icon_256x256.png": b"ic08",
    "icon_256x256@2x.png": b"ic14",
    "icon_512x512.png": b"ic09",
    "icon_512x512@2x.png": b"ic10",
}


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: pack-icns.py <iconset dir> <output.icns>", file=sys.stderr)
        return 2

    iconset, output = Path(sys.argv[1]), Path(sys.argv[2])
    entries = []
    for name, ostype in TYPES.items():
        path = iconset / name
        if not path.exists():
            continue
        data = path.read_bytes()
        if data[:8] != b"\x89PNG\r\n\x1a\n":
            print(f"ERROR: {name} is not a PNG", file=sys.stderr)
            return 1
        entries.append(ostype + struct.pack(">I", len(data) + 8) + data)

    if not entries:
        print(f"ERROR: no icons found in {iconset}", file=sys.stderr)
        return 1

    body = b"".join(entries)
    output.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)
    print(f"  {len(output.read_bytes()) // 1024:6d} KB  {output}  ({len(entries)} sizes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
