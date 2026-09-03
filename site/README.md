# Disk Space site

The marketing site, built with Next.js: prerendered pages plus one dynamic API
route that the apps use to check for updates.

```
bun install
bun run dev     # http://localhost:3070
bun run build
bun run start   # serves the build on http://localhost:3070
bun run lint    # eslint
bun run format  # eslint --fix, then prettier
```

Every page is prerendered at build time, but this is not a static export: the
one API route below runs per request, so the site needs a Node runtime to be
deployed on rather than a bucket of files.

## The version endpoint

`GET /api/latest-version?platform=macos&version=1.0.1`

The apps call this when they start and when their About dialog is opened. It
answers from [`lib/releases.ts`](lib/releases.ts), comparing the version passed
in against the newest release for that platform:

```json
{
  "downloads": "https://www.diskspace.io/#download",
  "platform": "macos",
  "latest": {"version": "1.0.1", "date": "…", "description": "…", "url": "…"},
  "updateAvailable": false,
  "current": "1.0.1",
  "currentDate": "2026-08-25"
}
```

`currentDate` is the release date of the version doing the asking, so the app
can say how far behind it is. Called with no `platform`, it answers for every
platform at once. It is `force-dynamic` and sends `Cache-Control: no-store`, so
each check is a real request that server-side analytics can count.

## Adding a release

Releases live in one file, [`lib/releases.ts`](lib/releases.ts), keyed by
platform. Add the new version as the **first** entry of its array, keeping the
rest in descending date order:

```ts
mac: [
  {
    version: "1.0.2",
    date: "2026-09-01",
    description: "What changed, in one line.",
    url: "https://…/DiskSpace-1.0.2.dmg",
  },
  …
]
```

Nothing else needs touching. The download buttons on the home page read the
first entry for each platform, `/releases` lists every entry, and the endpoint
above answers from the same data. A platform with an empty array renders a
greyed-out button labelled "Coming soon", which is how Windows and Linux appear
until they ship.

## The demo video

The home page reserves a full-width frame for a video, currently showing a
placeholder. To fill it, drop `demo.mp4` (and optionally `demo-poster.png`) into
`public/` and replace the placeholder with the `<video>` element in the comment
directly above it in [`app/page.tsx`](app/page.tsx).
