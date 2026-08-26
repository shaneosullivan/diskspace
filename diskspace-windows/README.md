# Disk Space for Windows

The Windows build of [Disk Space](../README.md): scan any folder or drive, see
where the space went, and act on it. Results appear while the scan runs rather
than after it.

Same design as the macOS version, and the same architecture — several I/O workers
feeding one applier thread that owns every mutation of a chunked struct-of-arrays
arena, so the UI reads without locking. What differs is everything below that
line, because NTFS is not APFS.

## Two backends

Windows offers two genuinely different ways to read a tree, and the app picks
between them.

| backend | mechanism | measured | needs |
| ------- | --------- | -------- | ----- |
| **`enum`** | `GetFileInformationByHandleEx(FileIdBothDirectoryInfo)` — one call returns a whole buffer of entries with their sizes, timestamps, attributes and file ids | 57k–460k entries/sec, depending on the tree | nothing |
| **`mft`** | reads and parses the NTFS master file table directly off the raw volume | 425k entries/sec — a whole drive in 1.69s | Administrator, NTFS |

`enum` is the direct analogue of the macOS `getattrlistbulk` path: a batched
metadata read with no per-entry `stat`. It is what runs by default, and it works
on every volume at any privilege level.

`mft` replaces millions of small metadata reads with a few thousand large
sequential ones, which is where its advantage comes from. It is chosen
automatically only when scanning a whole NTFS drive as Administrator; for a single
folder, reading the entire table to show one branch is slower than just walking
that branch, and the automatic choice should never be the slower one. The app
offers a one-click restart as Administrator when it would help, and never asks
again if told not to.

## Performance

AMD Ryzen 9 9955HX, NVMe, Windows 11, Defender real-time protection on, warm
cache. `diskscan bench` produced these.

**330,617 entries (`C:\Users\shane`)**

| workers | time | throughput |
| ------- | ---- | ---------- |
| 1 | 2.40s | 138k entries/sec |
| 2 | 1.37s | 241k entries/sec |
| **4** | **0.77s** | **431k entries/sec** |
| 8 | 0.76s | 438k entries/sec |
| 16 | 0.72s | 460k entries/sec |

**281,351 entries (`C:\Windows`)**

| workers | time | throughput |
| ------- | ---- | ---------- |
| 1 | 7.45s | 38k entries/sec |
| 2 | 5.89s | 48k entries/sec |
| **4** | **5.08s** | **55k entries/sec** |
| 8 | 4.97s | 57k entries/sec |
| 16 | 5.01s | 56k entries/sec |

Four workers is the knee in both cases, and past eight there is nothing left to
win. The default is therefore `min(8, cores - 2)`, leaving a core for the applier
and one for the UI.

**719,428 entries (all of `C:\`), both backends**

| backend | time | throughput | accounted for |
| ------- | ---- | ---------- | ------------- |
| `enum`, 8 workers | 5.18s | 144k entries/sec | 120.3 GB |
| **`mft`** | **1.69s** | **425k entries/sec** | **139.5 GB** |

Against 140.0 GB actually in use, the MFT backend accounts for 99.6%. The half a
gigabyte it does not reach is the NTFS structures that are not files with data
runs. The directory walk reaches 86% — everything a walk is able to see.

The gap between the two trees is worth reading carefully: the same machine and the
same code manage 431k entries/sec on a user folder and 55k on `C:\Windows`. That
is not the scanner. `C:\Windows` is dense with small files carrying long ACLs, and
every open goes past the real-time antivirus filter. It is the honest number for
that tree, and no amount of threading moves it.

## Accuracy

The figures are the point, so the cases that are easy to get wrong are handled
explicitly and tested. `tests\accuracy.ps1` builds a tree whose exact on-disk cost
is known by construction and asserts the totals to the byte — the Windows
equivalent of checking a scanner against `du`, except that the expected answer is
arithmetic rather than another tool's opinion.

- **Hard links are counted once.** Directory enumeration hands back a file id but
  no link count, so the only available test for "have I seen this before?" needs an
  entry per file rather than per multi-link file. That is what `IdMap` is: a flat
  open-addressed table at a load factor of 0.7, about 68 MB per four million files.
  The MFT backend does get a link count and only tracks the files that have more
  than one, exactly as the macOS version does with `st_nlink`.
- **Sparse and compressed files** report their laid-out length, not what they hold.
  On the `enum` path the two attributes that mark them trigger one
  `GetCompressedFileSize` call apiece, and nothing at all for ordinary files. On the
  MFT path nothing is taken from the attribute header at all: every non-resident
  attribute is measured by summing its actual data runs, on every extent. A hole
  has no runs, so it contributes nothing; a compressed attribute's runs are the
  compressed ones, so it reports what it really holds. Testing the sparse flag
  instead is not enough — `$BadClus:$Bad`, a hole spanning the entire volume, does
  not set it, and trusting its header adds the whole capacity of the disk to the
  total.
- **Reparse points are never followed.** Junctions, symbolic links and mounted
  volumes are recorded as entries and left closed: following one both double-counts
  its target and can send the walk round a cycle. There is deliberately no option
  to change that.
- **Long paths** go through `\\?\` throughout, so nothing stops at 260 characters.
- **Names are stored as UTF-16**, which is how NTFS hands them over and takes them
  back. It also makes the hot path a `memcpy` in both directions, and NTFS permits
  names containing unpaired surrogates that would not survive a round trip through
  UTF-8.
- **Resident files** — small enough to live inside their MFT record — occupy no
  cluster of their own, and are reported as occupying nothing, which is what they
  cost.

### What a scan cannot reach

Walking a whole drive, the total will not match the drive's used space, and the
app says so rather than leaving the difference unexplained. On this machine
`C:\` walks to 120.3 GB against 140.0 GB actually in use. The missing 20 GB is
190 directories that cannot be opened even as an administrator — `System Volume
Information` holds restore points and shadow copies, four of which are over a
gigabyte each — plus everything directory enumeration structurally cannot see:
`$MFT` itself (1.1 GB here), `$LogFile`, `$Secure`, and 420.9 MB of folder index
allocation.

Reading the file table finds all of it, which is the clearest argument for that
backend: same drive, same second, 139.5 GB instead of 120.3 GB.

Turning on "show other space" draws that remainder as a block in the treemap,
labelled *not reached by this scan*. The macOS version hides that block when a
whole volume is being shown, because on APFS there is nothing else to see; here
there is, so it stays.

### Where the two backends differ

Scanning the same drive both ways gives slightly different totals, and the larger
figure is the more truthful one. The MFT sees things directory enumeration cannot:
a folder's own index allocation, alternate data streams, and the volume's metadata
files. Those are real bytes on the disk. It also lists a hard-linked file at one of
its paths rather than at every path with the extras zeroed, so the totals agree but
the tree does not look identical.

## Settings and updates

**Settings** (File > Settings, or Ctrl+,) is a two-page window — General and
Treemap — drawn the same way as the panes, so it follows the theme like
everything else. There is deliberately no setting for the worker count: the
engine takes it from the number of cores the machine reports, and no value a user
could pick beats the measured knee.

**Updates** are checked once at launch, and again whenever About or Help > Check
for Updates is opened. This is the only network request the app ever makes: it
sends the platform and the version running, and nothing about what was scanned.
A newer version appears as a banner on the chooser. It can be switched off in
Settings, and a failed check is silent — a site that cannot be reached is not
something to interrupt anyone about.

Both honour `DISKSPACE_UPDATE_URL`, which points the check at a local copy of the
site, and `DISKSPACE_THEME=dark|light`, which forces a theme without touching the
machine's own setting. Both exist so this can be tested without changing anything
the user owns.

## Which Windows

**Windows 10 version 1607 or later**, and Windows 11. Read out of the binary
rather than assumed:

- The lowest OS-gated import is `GetDpiForWindow`, which arrived in Windows 10
  1607. That is the hard floor for the portable executable — an older release
  would fail to start rather than misbehave.
- The MSIX declares `MinVersion 10.0.17763.0`, Windows 10 1809, which is the
  floor for the Store build.

Both sit far below any serviced release: Windows 10 22H2 is the only version
still supported, and it is well past either.

**No Visual C++ Redistributable is needed.** The C runtime is linked statically,
so the only DLLs either binary imports are ones every Windows install already
has:

```
DiskSpace.exe   USER32 GDI32 SHELL32 ole32 COMCTL32 UxTheme dwmapi
                ADVAPI32 WINHTTP VERSION MSIMG32 KERNEL32
diskscan.exe    SHELL32 ole32 ADVAPI32 KERNEL32
```

That costs about 190 kB over linking the runtime dynamically, which is a fair
price for a download billed as portable actually running on a machine that has
never had a developer tool near it.

Two things degrade rather than break on older Windows 10, and neither touches a
figure the app reports:

- The **dark title bar** uses the DWM attribute numbered 20 from version 2004 and
  19 before it. Both are tried; older than either and the title bar stays light
  while the rest of the window follows the theme.
- **Dark scrollbars** need the uxtheme entry point added in 1809. It is looked up
  by ordinal and skipped when absent, leaving light scrollbars.

Everything else — both scan backends, the treemap, elevation, long paths — has no
version dependency beyond the floor above.

> Tested on Windows 11 (build 26200) only. The floors above are read from the
> binary's imports and the package manifest, which is a different thing from
> having run it on a Windows 10 machine.

## Building

Requires Windows 10 or later and the Visual Studio "Desktop development with C++"
workload. No CMake, no project files: the whole thing is a couple of dozen
translation units and a batch file anybody can read.

```
cd diskspace-windows
build.cmd
```

`build.cmd debug` builds unoptimised with symbols; `build.cmd cli` builds only the
command-line tool, which is the fast loop when working on the engine. Everything
lands in `build\`.

Run the app, optionally with a path to scan immediately:

```
build\DiskSpace.exe
build\DiskSpace.exe C:\Users\you\AppData
```

## Command line

`diskscan` exposes the same engine for scripting and benchmarking.

```
diskscan scan <path> [--workers N] [--backend auto|enum|mft] [--quiet]
                     [--verify] [--raw] [--no-hardlinks] [--no-compressed]
diskscan bench <path> [--repeat N] [--max-workers N]
diskscan iobench <path> [--workers N]
diskscan volumes
```

`scan` reports totals, per-kind statistics, the largest entries and a
stage-by-stage time breakdown. `--verify` additionally checks that a directory's
children occupy one contiguous run, that every parent points back correctly, and
that the rolled-up totals agree with a flat recount. `--raw` prints exact byte
figures one per line, for a script that has to compare numbers rather than read
them. `bench` compares thread counts; `iobench` prices buffer sizes, the
compressed-size lookups and hard-link accounting.

## Tests

```
tests\accuracy.ps1
```

Builds a tree containing a hard link, a directory junction, a sparse file, a path
past 260 characters and a name in three scripts, then asserts every total. Pass
`-Keep` to leave the tree behind for inspection.

`tests\ui-driver.ps1` is the other half: dot-source it for `Shot-App`,
`Shot-Window`, `Click-App`, `Click-WindowClient` and `Menu-App`, which locate the
app's windows by class name and drive them. It is how the screenshots in this
repository were taken and how the interface was checked in both themes.

## Differences from the macOS version

- No package concept. Windows has no equivalent of a `.app` bundle, so there is no
  "show package contents" setting.
- A folder's own size is zero on the `enum` backend: NTFS keeps a directory's index
  in its own record and does not report its allocation to a directory listing. The
  MFT backend does see it and counts it.
- "Added to the folder" is the creation time. Windows has no equivalent of
  `ATTR_CMN_ADDEDTIME`, and creation time behaves the same way for the recency
  buckets.
- The Size column is pinned inside the tree control rather than living in a second
  table beside it. AppKit needed two tables to hold a column still while the names
  scrolled; a custom-drawn pane simply does not move it.
- Settings live in one window reached from the File menu, which is where a Windows
  application puts them, rather than in a preferences window of its own.

The toolbar artwork is shared, not reimplemented: `collapse.png`, `side_hide.png`
and `side_show.png` are compiled in straight from `assets/images/`, so the two
applications cannot drift apart. They are monochrome strokes on transparency —
template images — so each is decoded at the size and colour it is about to be
drawn at and tinted through its own alpha, which is what lets one file serve both
themes and every scaling factor.

## Distribution

Two channels, from the same `build.cmd` output.

**Microsoft Store**, as an MSIX. One command builds a submission:

```
scripts\export-windows-store.ps1
```

It builds release, checks that the executable's own version resource agrees with
the package it is about to go into, runs the accuracy tests and refuses to
package if they fail, then writes `build\release\DiskSpace-<version>.msix`.

MSIX rather than MSI, and rather than an installer of any kind, because it is the
only format the Store signs for you. The Store does accept unpackaged `.exe` and
`.msi` submissions, but you sign those yourself, which puts you back where you
started. An MSI would earn its keep if there were services, drivers, file
associations or per-machine provisioning to do at install time; there is one
self-contained executable and nothing to decide.

`Packaging\make-msix.ps1` is the layer underneath, for working on the package
itself:

```
Packaging\make-msix.ps1 -Register        loose layout, needs Developer Mode
Packaging\make-msix.ps1 -Pack            just the package
Packaging\make-msix.ps1 -Pack -Sign      signs it with a local test certificate
```

`-Sign` is for installing it the way a user would, to look at the built article.
A submission must **not** be signed — the Store rejects a package that already
carries a signature.

**A plain `.exe`** from the site, for anyone who would rather not use the Store.
No installer: a disk analyser is reached for when a machine is nearly full or
misbehaving, and one portable file that runs without installing anything suits
that. This is the channel that needs its own Authenticode signature — see below.

### Releasing a new version

The version lives in one place: `src\app\product.h`.

```c
#define DISKSPACE_VERSION_MAJOR 1
#define DISKSPACE_VERSION_MINOR 0
#define DISKSPACE_VERSION_PATCH 0
#define DISKSPACE_VERSION_REVISION 0   // leave at 0
```

1. **Bump** `MAJOR`, `MINOR` or `PATCH`. Leave `REVISION` alone — the Store
   reserves the fourth component and rejects anything else. The Store also
   requires each submission to be strictly higher than the last, and nothing here
   knows what was uploaded before, so that part is yours to keep track of.

2. **Export.**

   ```
   scripts\export-windows-app.ps1
   ```

   It rebuilds from scratch (a stale object file would ship the previous version
   in the resource), checks the executable's version resource against the
   manifest it is about to go into, runs the accuracy tests, and refuses to
   package if any of that fails. Out come three files in `build\release\`:
   the `.msix`, the portable `.exe`, and `SHA256SUMS.txt`.

3. **Upload the `.msix`** — Partner Center ▸ your app ▸ Packages. Unsigned: the
   Store signs on acceptance and rejects a package that already carries a
   signature.

4. **Publish the `.exe` and the checksums** on the site, for the direct download.

5. **Add the release to `site/lib/releases.ts`.** Until that entry exists the
   in-app update check goes on telling every Windows user they have the latest
   version. It wants the three-component form — `1.0.1`, not `1.0.1.0` — which is
   what the app sends as `version=` and what the site publishes.

Everything in steps 2–4 follows from the one edit in step 1: the executable's
version resource (which is what the update check reports), the MSIX
`Identity/Version`, and the output filenames. A half-applied bump fails loudly
rather than shipping — the export compares the two and stops.

The macOS version is deliberately separate, in `APP_VERSION` and `BUILD_NUMBER`
near the top of `scripts/export-macos-app.sh`. The site keys releases per
platform, so the two are free to drift.

### Checksums

The export writes `SHA256SUMS.txt` beside the two artefacts, in the format
`sha256sum -c` reads. They are not a substitute for a signature — anyone able to
replace the download could replace the checksums with it — but they let a careful
person confirm that what arrived is what left, and they cost nothing:

```
Get-FileHash .\DiskSpace-1.0.0.0.exe -Algorithm SHA256
```

### Store identity

The three values Partner Center assigns live in
`Packaging\store-identity.txt`, which is not committed because it names an
account rather than the program. `store-identity.example.txt` shows the shape.
They exist as soon as the app name is reserved — Partner Center ▸ your app ▸
Product management ▸ Product identity — so there is no need to upload anything
first to obtain them. `Package/Identity/Publisher` is a GUID rather than a
readable company name, which is the one that catches people out.

The export names any value still left at a placeholder, because a package
carrying one is rejected on upload.
### Elevation survives packaging

The one thing worth verifying before committing to the Store, because it decides
whether the fast backend exists there at all: a packaged app is heavily
restricted from *being launched* elevated, and the app relaunches itself with
`ShellExecute "runas"` when reading the file table would help.

It works. Measured from a real install in `C:\Program Files\WindowsApps`, running
unelevated with package identity:

```
package identity   : ChofterApps.DiskSpace_1.0.0.0_x64__q26wzg14xnkgc
this process admin : False
runas result       : LAUNCHED
child is admin     : True
```

and the elevated instance scans the whole drive through the MFT backend exactly
as the unpackaged build does — 139.8 GB in 1.7s, "read from the file table".

The restriction that does apply: the exe inside `WindowsApps` cannot be launched
by an unrelated process, only from within the package identity. That is precisely
what the relaunch does, so it is not in the way.

## Shipping

The Store channel needs no certificate of your own — Microsoft signs submissions.

The direct `.exe` does. Windows needs Authenticode the way macOS needs
notarisation: unsigned, it is blocked outright by Smart App Control on a large
share of Windows 11 machines and SmartScreen-warned on the rest. Note the
asymmetry that makes shipping unsigned worse over time — an unsigned binary's
reputation attaches to its file hash and resets with every release, while a
signed one accrues to the certificate and carries across releases.

A self-signed certificate does **not** help here, however well it works for
sideloaded MSIX elsewhere: it chains to nothing, so SmartScreen treats it as
unknown, and asking the public to import a certificate is worse than the warning
it would avoid. What works is a certificate that chains to a Microsoft-trusted
root — an EV one is trusted immediately, an OV one has to build reputation first,
and Azure Trusted Signing rents the same trust monthly without a hardware token.
