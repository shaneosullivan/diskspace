import {
  latestDirectDownload,
  latestStoreListing,
  releases,
  type Platform,
} from "@/lib/releases";
import {downloadsUrl} from "@/lib/site";

// Runs per request rather than at build time, so every check is a real request
// that server-side analytics can see.
export const dynamic = "force-dynamic";

// The runtime names the apps report themselves as, which are not quite the keys
// the releases file uses.
const runtimeNames: Record<Platform, string> = {
  mac: "macos",
  windows: "windows",
  linux: "linux",
};

const platformsByRuntime = Object.fromEntries(
  (Object.keys(releases) as Array<Platform>).map(p => [runtimeNames[p], p]),
) as Record<string, Platform>;

function describe(platform: Platform) {
  const release = releases[platform][0];
  if (!release) {
    return null;
  }
  // The newest release does not necessarily carry both links - a store listing
  // can still be in review while its download is live - so each is resolved to
  // the newest release that actually has one. Null rather than undefined, so
  // the field is present in the JSON and a client can tell "no store listing"
  // from "this server is too old to know about store listings".
  return {
    version: release.version,
    date: release.date,
    description: release.description,
    url: latestDirectDownload(platform)?.url ?? null,
    sha256: latestDirectDownload(platform)?.sha256 ?? null,
    storeUrl: latestStoreListing(platform)?.storeUrl ?? null,
  };
}

/// Compares dotted numeric versions a component at a time, so 1.0.10 counts as
/// newer than 1.0.9 — which string comparison gets backwards.
function isNewer(candidate: string, current: string): boolean {
  const a = candidate.split(".").map(Number);
  const b = current.split(".").map(Number);
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    const x = a[i] || 0;
    const y = b[i] || 0;
    if (x !== y) {
      return x > y;
    }
  }
  return false;
}

export async function GET(request: Request) {
  const params = new URL(request.url).searchParams;
  const runtime = params.get("platform");
  const current = params.get("version");
  const platform = runtime ? platformsByRuntime[runtime] : undefined;

  const body: Record<string, unknown> = {downloads: downloadsUrl};

  if (runtime && !platform) {
    // An unknown runtime is not an error: a future build asking about a
    // platform this site has not heard of should be told there is nothing for
    // it, not handed a failure to interpret.
    body.platform = runtime;
    body.latest = null;
    body.updateAvailable = false;
  } else if (platform) {
    const latest = describe(platform);
    body.platform = runtime;
    body.latest = latest;
    body.updateAvailable = Boolean(
      latest && current && isNewer(latest.version, current),
    );
    if (current) {
      body.current = current;
      // The release date of the version doing the asking, so the app can say
      // how far behind it is rather than only that it is behind. Null when the
      // version is not one this site published.
      const mine = releases[platform].find(r => r.version === current);
      body.currentDate = mine ? mine.date : null;
    }
  } else {
    // No platform asked for: answer for all of them, which is what a browser
    // hitting this by hand wants to see.
    body.latest = Object.fromEntries(
      (Object.keys(releases) as Array<Platform>).map(p => [
        runtimeNames[p],
        describe(p),
      ]),
    );
  }

  return Response.json(body, {
    headers: {"Cache-Control": "no-store"},
  });
}
