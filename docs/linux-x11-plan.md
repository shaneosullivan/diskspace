# Replacing GTK with X11 — plan

Written 2 September 2026, after measuring what GTK actually costs to ship. The
target set for this work: **under 600 KB, self-contained, requiring nothing of
the host**, matching what the Windows and macOS builds already are.

## Why GTK has to go

Measured on Ubuntu 24.04 against the built binary, not estimated:

| Route | What the user installs | Needs from the host |
| --- | --- | --- |
| Current AppImage | 494 KB | GTK 4.10+, which no Xfce or KDE machine has |
| AppImage with GTK bundled | ~32 MB of libraries, ~13 MB compressed | nothing much |
| Flatpak | 276 KB + **1 GB** shared runtime | flatpak |
| **Xlib + Xft (this plan)** | **~400 KB** | libX11, libXft, libfontconfig, libfreetype |

The last row is the only one that meets the target, and the libraries it needs
are the ones every X11 desktop is built on: `libx11-6`, `libxrender1`,
`libxft2`, `libfontconfig1`, `libfreetype6`. Xfce and GTK 3 both depend on all
five, so Xubuntu has them before this is installed. Requiring those is not the
same kind of ask as requiring GTK 4.10, which splits the distribution list in
half by release date.

The Flatpak is the worst of the four by this measure and stops being the
recommended route. It stays buildable — the manifest is one file — for anyone
who wants sandboxing, with its limitations documented.

## What is actually being rewritten

Smaller than it sounds. `diskspace-linux/src/app` is 2,940 lines, of which
1,777 sit in files that touch `gtk_`. The rest — number formatting, the
palette, the non-UTF-8 name escaping — is toolkit-neutral and stays.

More importantly the drawing already does not use a toolkit. `core/treemap.cpp`
computes finished `0x00RRGGBB` pixels; GTK's only job was to put that buffer on
the screen. Swapping a Cairo surface for an `XImage` is the whole of that
change. The tree pane and the kind pane are custom-drawn lists already.

What GTK was genuinely providing, and what replaces it:

| GTK | Replacement | Notes |
| --- | --- | --- |
| Window, event loop, input | Xlib | The bulk of the new code |
| Pixel blit | `XImage` + `XPutImage` | `XShmPutImage` if the extension is there |
| Text | Xft | freetype and fontconfig underneath, both already required |
| Scrolled lists, buttons | drawn here | Already half-drawn here |
| File chooser | a small folder browser | See "The two awkward parts" |
| `GtkFileLauncher` | `xdg-open` | Part of xdg-utils, present on every desktop |
| Trash | the freedesktop trash spec, directly | ~60 lines; removes a dependency |

## The two awkward parts

### Text shaping

Xft renders through freetype and fontconfig, which covers Latin, Greek,
Cyrillic, CJK and anything else that does not need reordering. It does **not**
shape: Arabic letters will not join, and Indic clusters will not reorder. Pango
did that.

The fix would be to bundle HarfBuzz, which is 800 KB — larger than the entire
application, for a case that appears in filenames rarely. Not doing it. This
should be written down as a known limitation rather than discovered later, and
revisited if anyone reports it.

Font fallback does work, but it is not free, which this said before it was
built. Xft draws with one face and one face only: `XftDrawStringUtf8` has no
fallback in it, so a Japanese filename came out as a row of empty boxes even
with a CJK font installed. It is now done here — the text is split into runs of
characters one face can draw, and fontconfig is asked for a face for anything
the base font lacks, cached per font so a directory of Japanese filenames costs
one lookup rather than one per name.

The trap, for anyone who touches it: the fallback pattern must be built fresh.
Copying the base font's pattern and deleting the family from it looks right and
is not — that pattern is the *resolved* one, carrying `FC_FILE` and the rest of
the matched face's identity, so the search comes straight back to the font that
was just found not to have the character.

### The folder chooser

"Choose a Folder…" was a `GtkFileDialog`. There is no way to keep that without
either a toolkit or a portal, and the portal route reintroduces a host
requirement. So it gets written here: a list of directories, a path bar, double
click to descend, one button to accept. Perhaps 300 lines, and it draws with
the same list code the tree pane uses.

This is the single largest piece of genuinely new work in the plan.

## Order of work

Each step ends somewhere runnable, so a problem is found against a working
window rather than at the end.

1. **A window and a blit.** Xlib window, event loop, resize, and the treemap
   buffer on screen. No text, no interaction. Proves the hard part is easy.
2. **Text.** Xft with a font resolved through fontconfig, DPI from the
   `Xft.dpi` X resource. A measuring call, an ellipsising call, and a draw
   call — the three things every pane needs.
3. **Widget primitives.** Rectangle, rounded rectangle, line, the scrollbar,
   the button, hover and press states. Small, and everything else sits on it.
4. **The panes**, in the order they were built the first time: tree, treemap,
   kinds. Their layout logic is unchanged; only their drawing calls move.
5. **The chooser**, then the folder browser.
6. **Menus and actions.** Context menu, copy path, open, reveal, trash.
7. **Packaging.** The AppImage story becomes the honest one: one file, nothing
   to install, matching the Windows download.

## What this does not change

The scanner, the arena, the treemap geometry, the accuracy suite. All of it is
below the UI and none of it knows a toolkit exists. The suite must stay green
throughout, and it is the check that this rewrite has not touched anything that
matters for correctness.

## Known risks

- **Wayland-only sessions** reach this through XWayland. Present by default on
  GNOME and KDE, and Xubuntu's session is X11 anyway. A native Wayland backend
  is a second implementation of step 1 and is not in this plan.
- **glibc floor.** A binary built on Ubuntu 24.04 will not run on a distribution
  older than its glibc. That is a property of where it is built, not of this
  change, and the answer is an old build container rather than a code change.
- **HiDPI.** GTK handled scaling. Here it means reading `Xft.dpi` and scaling
  every metric, which is easy to get half-right and then find later at 200%.
  Worth testing deliberately rather than assuming.
