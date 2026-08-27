export type Platform = "mac" | "windows" | "linux";

export type ReleaseRecord = {
  version: string;
  date: string;
  description: string;
  /**
   * Direct download. Absent for a release that only ever shipped through a
   * store.
   */
  url?: string;
  /**
   * SHA-256 of the file at `url`. Worth publishing for an unsigned build, where
   * it is the only way anyone can check that what they downloaded is what was
   * uploaded. Take it from the artefact that was actually uploaded rather than
   * from a later rebuild: the compiler does not produce identical bytes twice.
   */
  sha256?: string;
  /**
   * Store listing. Absent until a submission is published, which can be days
   * after the download it covers is live — which is the whole reason the two
   * links are tracked separately rather than assumed to arrive together.
   */
  storeUrl?: string;
};

// Note that the first item in each array should be the latest release, and the
// rest should be in descending order of release date.
export const releases: Record<Platform, Array<ReleaseRecord>> = {
  mac: [
    {
      version: "1.0.4",
      date: "2026-08-26",
      description: "First public release of Disk Space for Mac.",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.0.4%2FDiskSpace-1.0.4.dmg?alt=media&token=6f366753-3c3f-4406-8598-ccef0761e144",
    },
  ],
  windows: [
    {
      version: "1.1.0",
      date: "2026-08-26",
      description:
        "First public release for Windows, where the app is called Disk Space ++. " +
        "Portable: one file, nothing to install, and no runtime to install either.",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.0.4%2FDiskSpace-1.1.0.0.exe?alt=media&token=b8419e55-098f-42b9-8e6b-698aba5a0137",
      sha256:
        "d87c3a841bfb71a21e9872bddaa9ba7546ba1e9cd2edc1945a3e932fe359ead5",
      // No locale in the link. The Store hands out URLs carrying whichever
      // market and language the person copying them happened to be browsing in
      // — ?hl=en-GB&gl=IE for this one — and publishing that would put every
      // visitor into an Irish English listing regardless of where they are.
      // Without it the Store picks for itself, which is what it is for.
      storeUrl: "https://apps.microsoft.com/detail/9nwpgvh9rp8l",
    },
  ],
  linux: [],
};

export const platformNames: Record<Platform, string> = {
  mac: "macOS",
  windows: "Windows",
  linux: "Linux",
};

/**
 * What each platform needs, for the line under the download buttons.
 *
 * The Windows floor is really version 1607, set by the lowest OS-gated call the
 * binary makes. Saying so under a download button would be noise - 22H2 is the
 * only serviced Windows 10 there is - so the exact figure lives in the Windows
 * README and this says the useful thing.
 */
export const platformRequirements: Record<Platform, string> = {
  mac: "macOS 15 or later",
  windows: "Windows 10 or later",
  linux: "Linux",
};

/**
 * Platforms someone can actually get a build for, in listed order.
 *
 * "Has a release" is not enough: a release with neither a download nor a store
 * listing is one nobody can install, and the button for it reads "coming soon".
 * This has to agree with that or the page promises what it does not offer.
 */
export function releasedPlatforms(): Array<Platform> {
  return (Object.keys(releases) as Array<Platform>).filter(
    platform =>
      Boolean(latestDirectDownload(platform)) ||
      Boolean(latestStoreListing(platform)),
  );
}

/** The rest: platforms still to come. The complement of the above. */
export function unreleasedPlatforms(): Array<Platform> {
  const out = releasedPlatforms();
  return (Object.keys(releases) as Array<Platform>).filter(
    platform => !out.includes(platform),
  );
}

export function latestRelease(platform: Platform): ReleaseRecord | undefined {
  return releases[platform][0];
}

/**
 * The newest release that can actually be downloaded directly.
 *
 * Not simply the newest release: the two ways of getting a build do not arrive
 * together. A store submission sits in review while the direct download is
 * already up, so each link is resolved to the newest release that has one
 * rather than to whichever release happens to be first in the list.
 */
export function latestDirectDownload(
  platform: Platform,
): ReleaseRecord | undefined {
  return releases[platform].find(release => Boolean(release.url));
}

/** The newest release with a published store listing. See above. */
export function latestStoreListing(
  platform: Platform,
): ReleaseRecord | undefined {
  return releases[platform].find(release => Boolean(release.storeUrl));
}

export function formatDate(date: string): string {
  // Parsed as UTC so the rendered date does not shift with the build machine's
  // time zone — the output has to be identical every time the site is exported.
  return new Date(`${date}T00:00:00Z`).toLocaleDateString("en-US", {
    year: "numeric",
    month: "long",
    day: "numeric",
    timeZone: "UTC",
  });
}

/// What each platform's app calls itself at runtime. Analytics and the version
/// endpoint both speak this vocabulary, so an event from the site and one from
/// an app describe the same platform the same way.
export const runtimeNames: Record<Platform, string> = {
  mac: "macos",
  windows: "windows",
  linux: "linux",
};
