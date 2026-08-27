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

`appimagetool` is not in the Ubuntu archive, and is itself an AppImage, so it
needs FUSE to run:

```bash
wget -O ~/.local/bin/appimagetool https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
chmod +x ~/.local/bin/appimagetool
sudo apt install libfuse2t64
```

Where FUSE is unavailable — a container, most CI runners — pass
`--appimage-extract-and-run` to it instead.

Neither artefact bundles GTK, so both need **GTK 4.10 or later** on the machine
that runs them. This is the one place the Linux build is weaker than the Windows
one, whose portable `.exe` links the C runtime statically and needs nothing at
all. Bundling GTK properly means `linuxdeploy` with its GTK plugin and about
eighty megabytes of libraries - a trade worth making deliberately rather than by
default.

## Flatpak

```bash
flatpak install -y flathub org.gnome.Platform//49 org.gnome.Sdk//49
flatpak-builder --user --install --force-clean \
    build/flatpak diskspace-linux/packaging/com.chofter.DiskSpace.yaml
flatpak run com.chofter.DiskSpace
```

The route worth taking for a release. The runtime carries its own GTK, so the
version floor below stops mattering: it installs the same way on Ubuntu 22.04 as
on a current Fedora.

Two things in the manifest are worth knowing about:

**`--filesystem=host`.** Read access to the whole filesystem is what a disk usage
tool is *for*; one confined to its own sandbox would report on nothing anyone
wants to know about. GNOME's own Disk Usage Analyzer holds the same permission.
It is read-*write* rather than read-only for exactly one feature — moving a file
to the trash. If that trade is not worth it, `--filesystem=host:ro` is a one-word
change and only the trash action stops working.

**No `--share=network`.** The update check is the only request this program ever
makes, and inside a Flatpak that is Flathub's job. The application detects the
sandbox and skips the check, so the permission would buy nothing and would have
to be explained to everyone who reads the list. The settings window says so
rather than leaving a switch that does nothing.

For a Flathub submission, swap the `dir` source at the bottom of the manifest for
the `git` source beside it — Flathub builds from a tagged commit, not a working
tree.

**What the sandbox can and cannot see.** Inside a Flatpak, `/usr` is the GNOME
runtime's rather than the machine's, and `/proc/mounts` describes the sandbox
rather than the disk. Left alone, the chooser offered `/var/cache` as though it
were the drive, and scanning `/` would have measured the runtime and called it
your files — a plausible-looking number that is entirely wrong, which is the
worst kind for this application to produce.

So when it detects the sandbox, the chooser stops reading `/proc/mounts` and
offers instead what `--filesystem=host` genuinely binds at its real path: the
home directory, and anything under `/mnt`, `/media` or `/run/media`. `statvfs` on
those reports the real filesystem, so the figures are right; the list is simply
shorter than it is natively. A scan of the same directory returns byte-identical
totals inside the sandbox and outside it.

One thing to know when rebuilding: `flatpak-builder` caches a `dir` source and
will happily reuse yesterday's copy of the working tree. Pass `--disable-cache`,
or the binary you test will not be the code you just wrote.

## The GTK version floor

Both the AppImage and the tarball need **GTK 4.10 or later** on the host. Fifteen
calls set that floor, in four clusters: `GtkAlertDialog` (the trash
confirmation), `GtkFileDialog` (the folder chooser), `GtkFileLauncher` (open and
show-in-files), and `gtk_widget_get_color`.

In practice that excludes Ubuntu 22.04 LTS and its derivatives — Mint 21,
Pop!_OS 22.04 — along with Debian 12. Everything current is fine: Ubuntu 24.04
and later, Debian 13, Fedora 38 and later, and the rolling distributions.

Every one of those fifteen calls has a pre-4.10 equivalent that still exists
(`GtkMessageDialog`, `GtkFileChooserNative`, `gtk_show_uri`,
`gtk_style_context_lookup_color`), so the floor could be dropped to 4.6 with
version guards. It is not worth doing while Flatpak solves the same problem
without putting deprecated code back in.

## Still to do
- The Windows build offers to restart itself as administrator when that would
  reach more of the disk. The equivalent here would be pkexec, and it is not
  wired up: run `diskscan` under sudo if a scan reports unreadable directories
  that matter.
