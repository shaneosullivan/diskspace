# Running the Linux build on this Windows machine

The artefacts `scripts/export-linux-app.sh` produces land in
`build/linux-release`, on the Windows drive. WSL can run them from there
directly — nothing needs copying, installing or unpacking first.

Everything below has been run as written on this machine.

## What is in the folder

```
build/linux-release/
  DiskSpace-1.0.0-x86_64.AppImage      one file, the whole application
  DiskSpace-1.0.0-x86_64.tar.gz        the same tree as a tarball
  SHA256SUMS.txt                       checksums for both
  com.chofter.DiskSpace.AppDir/        the unpacked tree the two are made from
```

The repository is at `/mnt/c/Users/shane/workarea/diskspace` inside WSL, so the
releases folder is `/mnt/c/Users/shane/workarea/diskspace/build/linux-release`.

## The short way

```powershell
.\scripts\run-linux-app.ps1
```

That is the whole thing. It finds the AppImage in `build/linux-release`, sets the
environment WSL needs, launches it detached, and tells you what to look for.

```powershell
.\scripts\run-linux-app.ps1 -Raise            bring the window to the front
.\scripts\run-linux-app.ps1 -Stop             close it
.\scripts\run-linux-app.ps1 -Cli volumes      run diskscan instead
.\scripts\run-linux-app.ps1 -Path /usr        start with a scan already going
```

Launching a second time does not open a second window: the application is
single-instance, so the script notices one is already up and raises it instead.

The window appears on the Windows desktop titled **`Disk Space (Ubuntu-24.04)`**.
WSLg prefixes it with `[WARN: COPY MODE]`, which is its own note about the
transport it uses and says nothing about the application. If it will not come to
the front when clicked in the taskbar — the penguin icon there is WSLg's stub,
not the window — use `-Raise`.

## The long way

Doing it by hand, if you would rather not use the script. Four environment
variables have to be set, and each earns its place; see the table below.

```bash
wsl -d Ubuntu-24.04 -- bash -c '
  cd ~
  export GDK_BACKEND=x11 DISPLAY=:0 GSK_RENDERER=cairo LIBGL_ALWAYS_SOFTWARE=1
  unset WAYLAND_DISPLAY
  APP=/mnt/c/Users/shane/workarea/diskspace/build/linux-release
  setsid nohup "$APP/DiskSpace-1.0.0-x86_64.AppImage" >/tmp/diskspace.log 2>&1 </dev/null & disown'
```

The command line tool needs no display and so no environment:

```bash
wsl -d Ubuntu-24.04 -- /mnt/c/Users/shane/workarea/diskspace/build/linux-release/com.chofter.DiskSpace.AppDir/usr/bin/diskscan volumes
```

`diskscan --version` answers `1.0.0`, which is the quickest way to confirm you
are running what you think you are.

## Why the environment matters

| Variable | Why |
| --- | --- |
| `DISPLAY=:0` | **The only one that still matters.** Where XWayland is listening, and the application speaks nothing else. |

The other three that used to be here - `GDK_BACKEND`, `GSK_RENDERER` and
`LIBGL_ALWAYS_SOFTWARE` - were all working around GTK, which is gone. There is no
Wayland surface to fail to composite and no GPU renderer to fall back from: this
is an X11 client that draws on the CPU everywhere, which is what it did on the
screen anyway.

`setsid nohup … & disown` is what keeps it alive after the `wsl` command that
started it returns. Without it the application dies with its parent shell.

## What to expect, and what not to read into it

**Scan `/` rather than `/mnt/c`** if you want a fair impression of the speed.
`/mnt/c` reaches the Windows drive through a 9p mount, which is several times
slower than a real filesystem — and it misreports sparse files as fully
allocated and gives no inode identity, so hard-link accounting cannot work
there either. None of that is the scanner; it is the mount. `/` is real ext4.

**The window is being remoted by WSLg**, so resizing will feel less smooth than
on real hardware. Scanning and hit-testing are unaffected, and the treemap is
drawn on the CPU on every platform regardless.

**This is not a fair look at the UI.** It is enough to try the interaction and
read the numbers, but the fonts, scaling and window behaviour are WSLg's, not a
Linux desktop's. The interface still deserves a look on real hardware before
release.

## Notes for anyone repeating this elsewhere

- **The executable bit survives on `/mnt/c`.** drvfs reports everything as
  `0777`, so the AppImage runs from the Windows drive without `chmod +x`. On a
  filesystem that does carry permissions — a USB stick formatted FAT, a file
  fetched by a browser — it will need one.
- **An AppImage needs FUSE.** WSL has none by default; `sudo apt install
  libfuse2t64` provides it. Without FUSE, `--appimage-extract-and-run` works
  anywhere and is what to use in a container or a CI runner.
- **The AppImage needs four libraries from the host**: libX11, libXft, libXrender
  and libfontconfig. Any machine with an X desktop has all four already, since
  Xfce and GTK 3 depend on every one of them. There is no version floor worth
  writing down.
