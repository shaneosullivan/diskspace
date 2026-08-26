# Disk Space

A free disk usage analyser for macOS and Windows: scan any folder or volume, see
where the space went, and act on it. Results appear while the scan runs rather
than after it.

![Main window](assets/images/mac_screenshot.jpg)

## Why

Existing tools of this kind block the interface while scanning, so a large volume
means several minutes of a spinner before anything is usable. This one publishes
results continuously — a partial tree and treemap are on screen within a second of
starting, and refine as the walk proceeds. Measured cost of updating live: under 1%.

## What it does

- **File kinds** — every file classified by extension, aggregated live, sortable.
- **Size-ordered tree** — folders ranked by size, with the Size column pinned so it
  stays visible however deep the names get.
- **Cushion treemap** — every file drawn proportional to its size, coloured by kind.
  Selecting a folder in the list outlines it in the map, so drilling down walks the
  highlight into smaller parts of the same picture instead of replacing it.
- **Four measures** — current size, plus bytes added in the last month, the last six
  months, and since this folder was previously scanned. Useful for answering
  "what has been growing?" rather than only "what is big?".
- **Hard links counted once**, and both logical and allocated size tracked. On a
  developer machine these differ enormously: 1.5 TB of logical data occupying
  313.9 GB of actual disk is normal once APFS clones and sparse files are involved.

## Performance

Measured on a 3.8M-entry tree (`~/Library`), Apple silicon, warm cache:

| configuration                    | time      | throughput           |
| -------------------------------- | --------- | -------------------- |
| `readdir` + `fstatat`, 1 thread  | 96.5s     | 39k entries/sec      |
| `getattrlistbulk`, 1 thread      | 78.4s     | 49k entries/sec      |
| `readdir` + `fstatat`, 4 threads | 30.6s     | 124k entries/sec     |
| **`getattrlistbulk`, 4 threads** | **24.2s** | **157k entries/sec** |

Four I/O workers is the measured optimum; eight is slower, because concurrent
metadata reads interfere at the device level rather than because of lock
contention (measured at 0.3% of worker time). Roughly 97% of the remaining time is
syscalls: `getattrlistbulk` 70%, `open` 27%. Larger read buffers and a trimmed
attribute set were both measured and made no difference.

Totals are verified byte-identical against `du` and `find`, including on a tree with
675,190 hard-linked duplicates, and are stable from 1 to 16 workers.

## How it is built

**Scanning.** Several I/O workers walk directories with `getattrlistbulk`, filling
thread-local batches. A single applier thread owns every mutation of the store, so
there is exactly one writer and the UI reads without locking.

**Storage.** A chunked struct-of-arrays arena: fixed 64K-node blocks that are
allocated once and never move, with readers gated on an atomically published node
count. Around 96 bytes per node, no per-node objects, no reference counting. Each
directory's entries land contiguously, so a node's children are an index range and
the outline view needs no auxiliary structure.

**Correctness while scanning.** Subtree totals roll up the parent chain as each
directory completes, so every figure is correct at every instant rather than only
at the end. Hard links are suppressed during the walk, before anything is rolled
up — dedupe afterwards and the totals are already inflated.

**Treemap.** Squarified layout, then cushion shading after
[van Wijk & van de Wetering](https://www.win.tue.nl/~vanwijk/ctm.pdf): a ridge is
added at every level of the hierarchy and accumulated, so directory structure shows
through the shading itself rather than needing borders. Layout and rasterisation run
off the main thread; an ID buffer is written alongside the pixels, making hit
testing a single array read regardless of how many rectangles are on screen.

## Windows

[`diskspace-windows/`](diskspace-windows/README.md) is the same application in
C++20 on Win32, sharing the architecture rather than the code: the same
workers-to-applier pipeline, the same chunked struct-of-arrays arena, the same
squarified cushion treemap with its ID buffer.

Everything below the arena differs, because NTFS is not APFS. There are two
backends — batched directory enumeration, which needs no privileges, and a reader
that parses the master file table directly, which scans a whole drive in seconds
but needs Administrator. The app runs unprivileged and offers to restart elevated
only when that would actually be faster.

Windows-specific accuracy work is documented there in full: hard links, sparse and
compressed files, reparse points, long paths and resident files each need handling
that has no macOS counterpart, and `tests\accuracy.ps1` asserts all of it against a
tree whose exact cost is known by construction.

## Building

**macOS.** Requires macOS 15 or later and a Swift 6 toolchain.

```
cd diskspace-macos
swift build -c release
```

Run the app, optionally with a path to scan immediately:

```
./.build/release/DiskSpaceApp
./.build/release/DiskSpaceApp ~/Library
```

Grant Full Disk Access to read protected folders; without it some directories are
skipped and totals come out low. The app says so when the permission is missing.

**Windows.** Requires the Visual Studio "Desktop development with C++" workload.

```
cd diskspace-windows
build.cmd
```

```
build\DiskSpace.exe
build\DiskSpace.exe C:\Users\you\AppData
```

## Releasing

`scripts/export-macos-app.sh` builds, signs, notarises and packages the macOS
release as a DMG. `scripts/export-windows-store.ps1` builds and packages the
Windows release as an MSIX for the Microsoft Store, which signs it on acceptance
— so that one needs no certificate, no notary account and no credentials.

Both run the checks before they package, and both refuse to produce anything if
those fail.

## Command line

`diskscan` exposes the same engine for scripting and benchmarking.

```
diskscan scan <path> [--workers N] [--backend bulk|stat] [--quiet] [--verify]
diskscan bench <path> [--repeat N] [--max-workers N]
diskscan iobench <path> [--workers N]
```

`scan` reports totals, per-kind statistics, the largest entries and a stage-by-stage
time breakdown. `--verify` additionally checks the contiguous-children invariant.
`bench` compares backends across thread counts; `iobench` prices buffer sizes,
attribute sets and `open` versus `openat`.

Windows ships the same tool with the flags that platform actually has — see the
[Windows README](diskspace-windows/README.md#command-line).

## Licence

Not yet chosen.
