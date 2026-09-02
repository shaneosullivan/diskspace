Disk Space 1.0.0 for Linux — x86_64
===================================

  DiskSpace-1.0.0-x86_64.AppImage    602K   one file, nothing to install
  DiskSpace-1.0.0-x86_64.tar.gz      478K   the same binaries, loose
  SHA256SUMS.txt


Running it
----------

    ./DiskSpace-1.0.0-x86_64.AppImage

That is all. It is built executable, so there is nothing to chmod, nothing to
install and nothing to add.

The one thing that undoes that is the stick itself: FAT32 and exFAT cannot store
the executable bit, so if this arrives on one of those the file lands without it.
Copying to the internal disk does not bring it back — the bit was lost when the
file was written to the stick, not when it was read off. In that case:

    chmod +x DiskSpace-1.0.0-x86_64.AppImage

Formatting the stick ext4 avoids the question entirely, if that is convenient.

If it exits complaining about libfuse.so.2, either install FUSE:

    sudo apt install libfuse2t64

or skip FUSE, which works anywhere:

    ./DiskSpace-1.0.0-x86_64.AppImage --appimage-extract-and-run


What it needs from the machine
------------------------------

Five shared libraries, and every one of them is what an X desktop is built on:

    libX11          libx11-6
    libXft          libxft2
    libXrender      libxrender1
    libfontconfig   libfontconfig1
    libpng          libpng16-16

Xfce depends on all of them, and so does GTK 3, so Xubuntu has them before this
arrives. Nothing needs installing. To satisfy yourself before running it:

    ldd DiskSpace-1.0.0-x86_64.AppImage 2>/dev/null | grep 'not found'

Silence means it will run.

There is no version floor worth writing down — the newest thing the application
uses is the X RENDER extension, which is from 2001. The one real floor is glibc,
and it comes from where this was built rather than from anything in the code:
built on Ubuntu 24.04, so it needs glibc 2.39 or later. Xubuntu 24.04 and newer
are fine. On 22.04 it will refuse to start, and the fix is a rebuild at this end
rather than anything you can do at that one.

Two optional things, neither of which stops it working:

  * xdg-open, for Open, Show in Files, and the links in the About box. Part of
    xdg-utils, which every desktop installs.
  * curl or wget, for the update check. Where neither is present the check does
    not happen and nothing is said about it.


Speed
-----

The scan runs several I/O workers feeding one applier thread, the same design as
the Windows and Mac builds. How many workers is chosen from the machine: the core
count, and whether the disk spins.

    solid state or virtual   between 8 and 32, following the core count
    spinning disk            between 2 and 4

Measured here on ext4 with the caches dropped, thirty-two workers is 2.2x the
eight the shared default used to insist on — a whole ext4 root went from 1.10s to
0.49s. The spinning-disk number is a guess: there was no such disk to measure on.

If your machine has one and this feels wrong, --workers overrides it:

    ./com.chofter.DiskSpace.AppDir/usr/bin/diskscan scan / --workers 8

and `diskscan bench /usr --max-workers 48` prints the whole curve.


The tarball, and the command line
---------------------------------

    tar xzf DiskSpace-1.0.0-x86_64.tar.gz
    ./com.chofter.DiskSpace.AppDir/usr/bin/diskspace

The same binaries, unwrapped, and they come out of the archive executable. Inside
is also `diskscan`, the scanning engine on the command line, which links no X
libraries at all and runs over ssh on a machine with no desktop:

    ./com.chofter.DiskSpace.AppDir/usr/bin/diskscan scan /usr
    ./com.chofter.DiskSpace.AppDir/usr/bin/diskscan volumes

Worth a run either way: it is the part shared with the Mac and Windows builds,
and it is what the accuracy suite drives.


Checking it against du
----------------------

This is the thing most worth doing on a machine I have never seen. The app
reports allocated blocks, which is what `du` reports and not what `ls -l` says:

    ./com.chofter.DiskSpace.AppDir/usr/bin/diskscan scan /usr --quiet | head -8
    du -sx --block-size=1 /usr

Those two should agree exactly. Here they do, on /usr and on a whole ext4 root.
If they disagree on your disk I would like to know by how much and on what
filesystem.

And to check what came off the stick is what went on it:

    sha256sum -c SHA256SUMS.txt


Known limitations
-----------------

  * No fast backend. On Windows the scanner reads the NTFS master file table
    directly, which is where most of its speed comes from. Linux has no portable
    equivalent — ext4, btrfs and XFS share no on-disk layout — so this walks
    directories with getdents64 and statx instead. Correct and quick; not the
    same jump.
  * Tested on ext4 only. btrfs in particular reports st_dev values that make
    subvolume boundaries look like mount points, and its compression puts
    st_blocks below the apparent size in ways ext4 never shows. If this Xubuntu
    install is on btrfs, treat the numbers as unverified and say so.
  * No text shaping. Latin, Greek, Cyrillic and CJK all render, and a font is
    substituted for anything the interface font lacks, but Arabic letters will
    not join and Indic clusters will not reorder. Keeping that would have meant
    bundling HarfBuzz, which is larger than this entire application.
  * X11 only. A Wayland-only session reaches it through XWayland, which is
    installed by default everywhere it matters. Xubuntu's session is X11.


Getting around
--------------

    F1              About
    Ctrl+O          choose a folder
    Ctrl+R / F5     scan again
    Ctrl+C          copy the selected path
    Ctrl+,          settings
    Backspace       back out of a folder on the map
    Delete          move the selection to the trash
    arrows          move around the list; right and left open and close

Right-click anything in the list or the map for the rest.


If something looks wrong
------------------------

The window can photograph itself, which is the most useful thing you can send
back:

    ./DiskSpace-1.0.0-x86_64.AppImage /usr --capture /tmp/shot.ppm --capture-after 3

It waits for the scan to finish, writes one frame and exits. Any image viewer
opens a PPM; `convert /tmp/shot.ppm /tmp/shot.png` if you would rather have a
PNG.

DISKSPACE_THEME=light or dark overrides the desktop's own setting, which is worth
trying if the colours look wrong — it reads the GTK settings file to decide, and
a desktop that records the preference somewhere else would fall back to light.


Not included: the Flatpak
-------------------------

There is a working Flatpak manifest in the repository, and it is deliberately not
on this stick. It carries 300 KB of application behind a one-gigabyte runtime
download, which is the wrong shape for a program whose whole argument is that it
is small — and it now solves a problem that no longer exists, since the version
floor it was built to sidestep went away with the toolkit.

It also cannot do the thing you would most want it for. Inside the sandbox the
host's /var is unreachable at any path, so a scan of "/" would silently omit it —
740 MB on this machine, and usually far more on a real one.
