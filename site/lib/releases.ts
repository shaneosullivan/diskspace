export type Platform = "mac" | "windows" | "linux";

export type ReleaseRecord = {
  version: string;
  date: string;
  description: string;
  url: string;
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
  windows: [],
  linux: [],
};

export const platformNames: Record<Platform, string> = {
  mac: "macOS",
  windows: "Windows",
  linux: "Linux",
};

export function latestRelease(platform: Platform): ReleaseRecord | undefined {
  return releases[platform][0];
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
