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
      version: "1.0.1",
      date: "2026-08-25",
      description: "First public release of Disk Space for Mac.",
      url: "https://github.com/shaneosullivan/diskspace/releases/download/v1.0.1/DiskSpace-1.0.1.dmg",
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
