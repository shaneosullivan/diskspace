# Working in this repository

## Verifying a Mac release

The release is built by `scripts/export-macos-app.sh`, which sets the version at
the top of the file, then builds, signs, notarizes, staples and writes the DMG
and the Homebrew cask.

The Apple ID it notarizes under is not in the repository, which is public. It
lives in `.env`, ignored by git, and the script stops before doing anything if
that file has not been sourced:

```bash
cp .env.example .env    # first time only, then fill it in
set -a; . ./.env; set +a
./scripts/export-macos-app.sh
```

**Never judge a release by running the app out of `build/release` or `.build`.**
This is the single most important rule here, and it has already cost a shipped
release. Version 1.1.1 launched perfectly on the machine that built it and
crashed on launch for every other Mac in the world: SwiftPM's `Bundle.module`
looks for resources at an absolute path inside the build directory, that path
existed here and nowhere else, and the generated accessor calls `fatalError`
when it finds nothing. The app was "tested" and the test proved nothing, because
it was run from the one place the bug could not appear.

So verification means running the app the way a user gets it: out of the DMG,
copied somewhere else, with the image detached.

```bash
VERSION=1.1.3   # match scripts/export-macos-app.sh

# Any stale mount is a different version wearing the same volume name, and
# copying from it silently tests the previous release instead of this one.
for v in /Volumes/Disk\ Space*; do [ -e "$v" ] && hdiutil detach "$v" -quiet; done

hdiutil attach "build/release/DiskSpace-$VERSION.dmg" -nobrowse -quiet
rm -rf /tmp/dsverify && mkdir -p /tmp/dsverify
cp -R "/Volumes/Disk Space/Disk Space.app" /tmp/dsverify/
hdiutil detach "/Volumes/Disk Space" -quiet

defaults read "/tmp/dsverify/Disk Space.app/Contents/Info.plist" CFBundleShortVersionString
codesign -v --deep --strict "/tmp/dsverify/Disk Space.app" && echo "signature ok"
spctl -a -t exec -vv "/tmp/dsverify/Disk Space.app"   # expect: source=Notarized Developer ID

open /tmp/dsverify/Disk\ Space.app
```

Then confirm, by looking at the running copy and not by assuming:

- It opens a window at all. A missing resource is a launch crash, not a blemish.
- The version in the About window is the one you just built.
- A scan runs: pick a small folder such as `/Applications` and check the tree,
  the treemap and the toolbar glyphs all draw. The toolbar images live in the
  resource bundle, so they are what fails first when packaging is wrong.

Use `open` rather than launching the binary from a shell. Full Disk Access is
checked by probing a TCC-protected path, so a binary started from a terminal
inherits the terminal's grant and reports access the shipped app will not have.

Finally, read the export script's output to the very end. It runs under
`set -euo pipefail`, and the Homebrew cask is written *after* notarization: on
1.1.2 a `grep` for a version not yet in `site/lib/releases.ts` returned non-zero
and killed the script silently, after a good DMG had already been produced. A
successful DMG is not evidence that the rest of the run succeeded.

The cask's url is a placeholder until the release exists in
`site/lib/releases.ts` or `DISKSPACE_DOWNLOAD_URL` is set. Publish the DMG, add
the entry with its sha256, and run the script again to get a cask worth
committing to the tap.
