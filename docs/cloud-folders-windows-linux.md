# Cloud folders on Windows and Linux

The Mac app recognises folders whose contents are not on the disk — iCloud
Drive, Dropbox, OneDrive, anything that registers a File Provider domain — and
can leave them unwalked. Windows and Linux do none of this yet. This is what
that work involves, written so it can be picked up on a machine that can build
those targets. Nothing here has been compiled or run; a Mac cannot build either.

The macOS work is commits `14ae229`, `b1fbf22`, `75b4433`, `bf43028`,
`a117e94`, `74fd3b3`, `564e80e`. Read them in order — the last four are fixes
from four rounds of review, and they are where the sharp edges are.

## What the feature does

1. **A placeholder file contributes no logical size.** Its allocated size is
   already zero, so the default figure was never wrong; the logical one had
   folders holding gigabytes that no disk anywhere was holding.
2. **Provider folders and placeholder files are labelled** in the tree —
   `cloud storage`, `cloud storage · not scanned`, `online only`. A folder full
   of files that weighs nothing otherwise looks like a bug in the program rather
   than a fact about the disk.
3. **They can be left unwalked**, from a setting or from an offer that appears
   mid-scan when the walk is visibly stuck in one. Off by default: a synced
   folder is not necessarily empty of real bytes, and refusing to look
   under-reports the very thing the app is for.
4. **A scan can stop waiting** for workers parked inside an uninterruptible
   directory read, because that is the only way to end one. Measured against a
   real provider, the wait after asking to skip went from 123 seconds to about
   two.

## How much is shared

More than it looks. `core/` is common to both targets — `core/tree_scanner.cpp`
holds `walk`, `workerLoop` and `applierLoop`, `core/queues.h` holds `DirQueue`,
`core/node_arena.h` holds the flags. Only `readDirectory` is per-platform
(`diskspace-windows/src/core/tree_scanner_win.cpp`,
`diskspace-linux/src/core/tree_scanner_posix.cpp`).

So **the skipping, the abandonment and the accounting are one port into shared
code**, and only the detection is written twice. The macOS versions of those
pieces are in `diskspace-macos/Sources/DiskScanCore/TreeScanner.swift` and
`Queues.swift`, and they translate closely.

## Detection

### Windows

Per file, straight from `FindFirstFileEx` — no extra open, no extra call:

| Signal | Meaning |
|---|---|
| `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS` (`0x00400000`) | The modern dehydrated placeholder. The single best indicator. |
| `FILE_ATTRIBUTE_RECALL_ON_OPEN` (`0x00040000`) | Older form of the same idea. |
| `FILE_ATTRIBUTE_OFFLINE` (`0x1000`) | Legacy HSM. |
| `FILE_ATTRIBUTE_REPARSE_POINT` + a tag in the `IO_REPARSE_TAG_CLOUD` family (`0x9000001A`, plus `_1`…`_F`) | Cloud Files API placeholder. `WIN32_FIND_DATA.dwReserved0` carries the tag when the attribute is set. |

For the tree root: **`CfGetSyncRootInfoByPath()`** answers "is this path inside a
cloud sync root", which is the equivalent of the extended attribute the Mac
uses. `mft_scanner.cpp` reads attributes from the MFT and will need the same
treatment as the `FindFirstFileEx` path — check both.

The allocated-vs-logical distinction the Mac relies on holds here too: a
dehydrated placeholder has `EndOfFile` at its full size and `AllocationSize` at
or near zero.

### Linux

There is no placeholder attribute; cloud drives are ordinary mounts, usually
FUSE. So the unit is the *mount*, not the file:

- `statfs.f_type`: `FUSE_SUPER_MAGIC` `0x65735546`, and the network types —
  NFS `0x6969`, SMB/CIFS `0xFF534D42`, 9P `0x01021997`, CEPH `0x00c36400`.
- `st_dev` changing across a boundary, which **does** work here. It does not on
  macOS, which is the single most surprising thing in the Mac implementation:
  a File Provider domain reports the same `st_dev` as the home folder, no entry
  in the mount table, `apfs` from `statfs`, and `MNT_LOCAL` set. All of that was
  measured, not assumed.
- `/proc/self/mountinfo` for the filesystem type and source, if a name is wanted
  for the interface.

`st_blocks == 0 && st_size > 0` is a useful cross-platform hint but not
sufficient alone — sparse files look identical.

Note that `tree_scanner_posix.cpp` already stops at a device change — a
directory whose `st_dev` differs from its parent's is marked `IsMountPoint` and
not descended into (around line 224). So a FUSE or network mount is *already*
excluded from a scan of the home folder, unconditionally. What is missing on
Linux is mostly the labelling and the accounting, not the skipping. Check that
before writing much: the Linux job may be considerably smaller than the Windows
one, and may amount to naming what is already being left out.

## The flag numbering collides — decide this first

`core/node_arena.h` already uses bits 0–5, with **`IsUnreadable = 1 << 4`**. The
Mac uses:

```
isCloudPlaceholder = 1 << 6
isCloudRoot        = 1 << 7
isCloudUnread      = 1 << 4     // collides with IsUnreadable on the C++ side
```

Bits 6 and 7 are free in C++ and can take the same meanings, which is worth
doing — the two implementations are already numbered alike deliberately. The
third has nowhere to go: C++ has no free bit left, so either widen `flags` to
`uint16_t` or fold "unread" into something else. Do not quietly give bit 4 two
meanings across the two codebases; that is exactly the trap the existing comment
in `node_arena.h` was written to prevent.

## The parts that were hard, so they are not rediscovered

**Probing costs real time.** Asking the filesystem whether each directory is a
provider root cost 14–20 µs warm and 31 µs cold, measured over 205,856
directories — about twenty CPU-seconds across a home folder, to prove that
almost nothing is one. macOS puts every domain in one place, so the Mac only
asks about directories sitting directly inside `~/Library/CloudStorage`, plus
the scan root and its ancestors. Windows will want the same discipline around
`CfGetSyncRootInfoByPath`. Measure before and after; the Mac change came out at
359k entries/sec either way.

**Never open or read a placeholder to test it.** That can ask the provider to
fetch the file, which is the opposite of what a program measuring a disk should
do. Attributes and extended attributes only.

**A thread inside a directory read cannot be interrupted.** Skipping stops new
descents at once, but a worker already inside the call has to come back on its
own, and that is where the two minutes went. The Mac's answer is to stop waiting
for it: the walk gives up, the worker finds its walk is over and drops what it
is holding. Getting that right took three attempts and the details matter:

- Each worker must hold **its own walk's batch queue**, not read it off the
  scanner, or a straggler grafts a directory from an abandoned tree onto a live
  one — wrong sizes under the wrong parent, with nothing to show why.
- Each walk needs an **epoch** the worker checks before touching anything
  shared.
- **The decision and the act must be one critical section.** Asking under the
  queue's lock and cancelling afterwards left a gap in which a worker could
  finish a genuine directory and have its subdirectories stranded. `DirQueue`
  answers and cancels together (`cancelIfOnlyProviderWorkOutstanding`).
- The condition is: skipping asked for, and `pending == holders.count`, and
  every holder is inside a provider. `pending` counts everything discovered and
  not yet applied, so equality means nothing is queued and nothing is waiting to
  be recorded. One worker on an ordinary local directory vetoes the whole thing.
- It must hold for **two seconds of polling**, not one sample. The condition
  flickers true every time the applier drains the stack before pushing a batch's
  children.

**Say what is missing.** A total with a tree absent from it is wrong unless the
interface admits it. Count providers, not directories: skipping one folder once
reported "2054 online folders not counted", which is the number of things inside
it and no use to anyone. On a refresh the count cannot be replaced or maximised
— keep the set of provider nodes, because a refresh knows nothing about
providers elsewhere in the tree.

## Tests

The Mac target has 64 tests in `diskspace-macos/Tests/DiskScanCoreTests/`; the
C++ targets have `diskspace-linux/tests/` and `diskspace-windows/tests/`. Worth
mirroring, in rough order of value:

1. **The skip path itself** — a tree walked with and without skipping, asserting
   the provider's bytes are in one and not the other. `ScanOptions` on the Mac
   grew an injected list of provider roots purely so this could be tested
   without a provider installed; the same trick applies.
2. **The flags** — a skipped folder is marked unread, a walked one is not, and
   an ordinary empty folder is neither.
3. **The prefix boundary** — `…/Dropbox Archive` must not be skipped along with
   `…/Dropbox`.
4. **The abandonment predicate**, as a function of values rather than by
   arranging a live scanner.
5. **Reading the scanner while it scans**, which is what the window does ten
   times a second. On the Mac this caught two data races nothing else reached.

Run the C++ suites under `-fsanitize=thread` if the toolchain allows. On macOS
`swift test --sanitize=thread` went from thirteen race sites to zero, and found
more than either human-style review did.

**Mutation-test the result.** Break each decision in a scratch copy and check a
test fails. On the Mac, five fixes were shipped with tests that did not fail
when the fix was reverted — they were found this way and only this way.

## Known gap, inherited

Removing `dirs.released(index)` from the Mac scanner passes all 64 tests.
Covering it needs an abandonment driven from a live walk, and a directory that
blocks for seconds cannot be staged on a build machine. If the Windows or Linux
environment can produce one — a genuinely slow network mount, or a FUSE
filesystem written to stall — that gap can be closed there in a way it cannot
be here, and it is worth doing.
