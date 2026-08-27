# Disk Space for Linux

The same scan engine as the macOS and Windows builds, behind GTK4. Called plain
"Disk Space" here — the Windows build is "Disk Space ++" only because the name
was already taken on the Microsoft Store.

## Building

```bash
sudo apt install build-essential cmake ninja-build pkg-config libgtk-4-dev
cmake -S diskspace-linux -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Two binaries come out:

- `diskspace` — the application.
- `diskscan` — the same engine on the command line, for scripting and for
  measuring. `diskscan scan /usr`, `diskscan volumes`, `diskscan bench`.

GTK 4.10 or later. Ubuntu 24.04 ships 4.14, which is what this is developed
against.

## What is shared, and what is not

`core/` at the repository root holds everything that was never
platform-specific: the arena, the queues, the batches, the hard-link map, the
file-kind table, the treemap layout, and about four hundred lines of the scanner
itself — the applier, the roll-ups, the rescan paths. All of it is compiled
unchanged into the Windows build.

What lives here is what genuinely differs:

| File | What it does |
| --- | --- |
| `src/core/tree_scanner_posix.cpp` | The clock, the directory open, the directory read |
| `src/core/scan_history_posix.cpp` | Baselines under `$XDG_DATA_HOME/diskspace` |
| `src/platform/posix_util.cpp` | `/proc/mounts`, `statvfs`, path canonicalisation |
| `src/app/` | The GTK4 window |

## Two differences worth knowing about

**There is no fast backend.** On Windows, reading the NTFS master file table
directly is roughly an order of magnitude faster on a whole volume. Linux has no
counterpart: ext4, btrfs and XFS share no on-disk layout, so matching it would
mean three hand-written filesystem parsers, each a correctness risk on somebody
else's data. This uses `getdents64` plus one `statx` per entry, which is what
`du` and `ncdu` do, across the same worker pool. It reads about 240,000 entries a
second cold on an NVMe-backed volume.

The `statx` calls are issued against the open directory's descriptor with a
relative name, so the kernel resolves no path per file, and with
`AT_STATX_DONT_SYNC`, so a network filesystem is not asked to make a round trip
for each one.

**Names are bytes.** A Linux filename is not required to be valid UTF-8, and
frequently is not. The scanner stores exactly what the kernel handed over — see
`core/name_string.h` — because those bytes are what has to go back to `open()`.
Only the display layer is lossy, escaping the invalid bytes the way `ls` does so
the row still appears.

## Accuracy

```bash
diskspace-linux/tests/accuracy.sh
```

Twelve checks: hard links counted once, sparse files, a symlink pointing out of
the tree, a directory symlink loop, a name that is not valid UTF-8, a path past
260 characters, mount-point detection, and the total checked against `du -sx`
byte for byte.

**It must run on a real filesystem.** Under WSL that means somewhere in `$HOME`.
A tree built on `/mnt/c` reports every sparse file as fully allocated and gives
no inode identity, so half of these would fail for reasons that have nothing to
do with the scanner. The script refuses to run there rather than reporting
nonsense.

On `/usr` the scanner and `du -sx` agree exactly, at 1,906,540,544 bytes, once
`du`'s count of directories' own blocks is subtracted — the scanner reports only
what is inside a directory, as the Windows and macOS builds do.

## Mount points

A directory on a different device is recorded and never descended into, which is
what the Windows build does with a mounted volume. Following it would count a
disk that is scannable in its own right twice, and can send the walk round a
cycle.

The volume chooser lists only what can actually be freed: no overlays, no
squashfs, no read-only mounts, and no tmpfs except `/tmp`.

## Packaging

```bash
./scripts/export-linux-app.sh
```

The counterpart of `export-macos-app.sh` and `export-windows-app.ps1`: reads the
version from `product.h`, builds release, cross-checks what the binary reports
against what was meant to be built, runs the accuracy suite against that binary,
assembles an AppDir, and writes a tarball and `SHA256SUMS.txt`. It builds an
AppImage too when `appimagetool` is on the path, and says so plainly when it is
not rather than failing.

Neither artefact bundles GTK, so both need **GTK 4.10 or later** on the machine
that runs them. This is the one place the Linux build is weaker than the Windows
one, whose portable `.exe` links the C runtime statically and needs nothing at
all. Bundling GTK properly means `linuxdeploy` with its GTK plugin and about
eighty megabytes of libraries - a trade worth making deliberately rather than by
default.

## Still to do

- Flatpak for Flathub, which is the route most people will want and the one that
  solves the GTK version floor properly.
- The Windows build offers to restart itself as administrator when that would
  reach more of the disk. The equivalent here would be pkexec, and it is not
  wired up: run `diskscan` under sudo if a scan reports unreadable directories
  that matter.
