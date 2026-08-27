# Disk Space for Linux — plan

Written 26 August 2026, immediately before the reboot that finishes the WSL2
install. If a session picks this up cold, start at "Right now".

## Right now

`wsl --install --no-launch` has been run from an elevated terminal and asked for
a reboot. Nothing else has been done: there is no Linux source tree, no branch,
no build.

After the reboot, in order:

```bash
wsl --list --verbose          # expect Ubuntu, VERSION 2, State Stopped
wsl --set-default-version 2
```

If `--list` still reports that WSL is not installed, the feature enable did not
survive the reboot — check `VirtualMachinePlatform` from an elevated prompt with
`Get-WindowsOptionalFeature -Online -FeatureName VirtualMachinePlatform`.

The distro was installed with `--no-launch`, so no user account exists yet and
the first launch would otherwise drop into an interactive prompt that an agent
cannot answer. Create the account non-interactively instead:

```bash
wsl -d Ubuntu -u root -- bash -lc 'id shane || (useradd -m -s /bin/bash -G sudo shane && printf "[user]\ndefault=shane\n" > /etc/wsl.conf)'
wsl --shutdown
```

Set a password with `wsl -d Ubuntu -u root -- passwd shane` when convenient;
sudo will ask for it. Then the toolchain:

```bash
sudo apt update && sudo apt install -y build-essential clang cmake ninja-build pkg-config libgtk-4-dev git
```

Claude Code installs inside the distro and drives the loop from there.

**The one trap that matters for this project:** the distro's root filesystem is
real ext4 in a VHDX, so hard links, sparse files, `statx` block counts and mount
boundaries all behave correctly — that is where every test corpus belongs.
`/mnt/c` is a translation layer with no true inode identity and unreliable
`st_blocks`. Accuracy tests run there would be wrong, not merely slow. Keep
everything under `~`.

## What is already portable

The Windows core is roughly 2,700 lines, and the split is better than it looks.
Counting Windows-specific references per file:

| File | Windows refs | Verdict |
| --- | --- | --- |
| `chunked_array.h`, `id_map.h`, `queues.{h,cpp}` | 0 | Move as-is |
| `batch.h`, `file_kind.h` | 2 | Trivial |
| `node_arena.{h,cpp}` | 7 | Needs the string decision below |
| `scan_history.cpp`, `tree_scanner.{h,cpp}` | ~25 each | Needs a POSIX backend |
| `win_util.cpp` | 36 | Platform layer, rewrite per platform |
| `mft_scanner.cpp` | 26 | **No Linux equivalent — see below** |

The treemap itself (`treemap.cpp`, 506 lines: squarified layout plus van Wijk
cushion shading) is pure geometry over the arena and should port with only the
final blit swapped out.

## The three decisions to make first

### 1. Filename encoding — decided, and not the way this first said

`NodeArena` stores names as UTF-16 because that is what Win32 hands back. Linux
filenames are arbitrary byte strings with no encoding guarantee: they are not
required to be valid UTF-8, and a scanner that assumes otherwise will corrupt or
drop real files.

This originally recommended moving the arena to UTF-8 and converting on the
Windows side. Two things found while reading the code say otherwise:

- `node_arena.h` already documented the objection: NTFS permits names containing
  **unpaired surrogates**, which have no UTF-8 representation. Such a name could
  not survive the round trip back to the call that has to open the file.
- `tree_scanner.cpp` rebuilds each directory's path **out of the arena** before
  opening it. Names are read back on the hot path, not just written to it, so a
  stored encoding needing conversion would pay for it once per directory in each
  direction.

**Decided instead: store what the operating system gave.** `NameChar` is
`wchar_t` on Windows and `char` on Linux, with a `DS_T()` literal macro and
`toUtf8`/`fromUtf8` used only at the display boundary, where a toolkit demands
its own encoding anyway and lossiness is harmless. This is the arrangement Rust
reaches for with `OsString`. Both platforms keep an exact round trip, both hot
paths stay a memcpy, and on Windows the change is a rename rather than a change
in behaviour — which the existing accuracy suite confirmed.

Argued at length in `core/name_string.h`.

### 2. No MFT equivalent exists

The Windows speed story rests on parsing the master file table directly. Linux
has no portable counterpart: ext4, btrfs and XFS have entirely different on-disk
layouts, so matching it would mean three separate backends, each parsing a
filesystem by hand, each a correctness risk on someone else's data.

**Recommendation:** do not attempt it. The Linux backend is `getdents64` plus
`statx(AT_STATX_DONT_SYNC)` across the existing thread pool — essentially the
`enum` backend, which is already fast. Consider `io_uring` later as an
optimisation, not as a first cut. Set expectations accordingly: the Linux build
will not show the jump the MFT backend gave Windows, and the site should not
imply it does.

Accuracy is actually *easier* than on Windows:

- Hard links: dedup on the `(st_dev, st_ino)` pair — the same `IdMap` idea, with
  the two fields hashed together.
- Sparse and compressed files: `st_blocks * 512` is the allocated size directly,
  with none of the `GetCompressedFileSizeW` fixups the Windows path needs.
- Mount boundaries: refuse to cross a changed `st_dev` unless the scan is of a
  whole volume, and skip `/proc`, `/sys`, `/run` and `/dev` always.
- Symlinks: never follow, matching the existing reparse-point policy.

### 3. UI toolkit — the real cost

This would be the third native toolkit after Cocoa and Win32. The app draws its
own treemap onto a canvas and owns its list rendering, so what is needed is a
window, a scrollable list, a drawing surface and a menu — not a large widget
vocabulary.

**Recommendation: GTK4.** Native on GNOME, good Wayland support, LGPL, and
`GtkSnapshot`/Cairo maps cleanly onto how `treemap_view.cpp` already works.
Qt6 is the alternative and is more capable, but heavier to ship and its widget
model buys little here.

Expect the UI to be most of the work: `src/app` is ~5,900 lines against ~2,700
for the core.

## Suggested order

1. **UTF-8 arena change on `main`**, verified by the existing Windows accuracy
   suite. No Linux code involved.
2. **Branch `linux-app`.** Lift the portable core into a shared location. This is
   also the moment to decide whether the tree becomes `src/core` shared by three
   platforms or stays duplicated — three copies of `IdMap` is the wrong answer.
3. **CLI first, no UI.** Port `diskscan` (406 lines) with the POSIX backend
   behind it. This is the whole scanning engine, testable and benchmarkable with
   no toolkit involved, and it is what the accuracy suite drives.
4. **Port the accuracy suite** from PowerShell to shell or a small C++ harness:
   hard links, sparse files, bind mounts, deep paths, non-UTF-8 filenames,
   mount-boundary behaviour. A non-UTF-8 filename case is essential and has no
   Windows counterpart.
5. **GTK4 shell**: window, drive chooser, tree pane, treemap canvas. Port
   `treemap.cpp` geometry unchanged and re-target only the blit.
6. **Packaging**: Flatpak for Flathub as the primary route, plus an AppImage so
   the direct download matches the Windows "one file, nothing to install" story.
   `.deb`/`.rpm` only if asked for.
7. **Site**: `unreleasedPlatforms()` in `site/lib/releases.ts` already drives
   both the requirements line and the "on the way" note, so publishing a Linux
   release is a matter of adding the record — no copy to hunt down.

## Known risks

- **WSL2 is not a benchmark environment.** I/O goes through a VHDX. Fine for
  relative comparisons while optimising; not a source of a files-per-second
  figure to publish beside the Mac one. That needs a Hyper-V VM or real
  hardware.
- **WSLg is not a real desktop.** Good enough to develop a GTK4 UI and take
  screenshots; HiDPI, fractional scaling and compositor behaviour differ. Final
  UI judgement needs a full GNOME session.
- **Filesystem variety.** btrfs subvolumes report `st_dev` values that make
  subvolume boundaries look like mount points, and its compression means
  `st_blocks` is below apparent size in ways ext4 never shows. Test on btrfs
  before claiming support.
- **Scope.** The Windows port was mostly a UI rewrite over a portable core. Linux
  is that again, plus the encoding change, minus the fast backend.
