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
  /**
   * The same build in other formats, beside the one at `url`. Linux ships three
   * ways - a .deb, a tarball and an AppImage - and `url` is the recommended one.
   */
  alternatives?: Array<AlternativeDownload>;
};

export type AlternativeDownload = {
  format: "tarball" | "appimage";
  url: string;
  sha256: string;
};

// Note that the first item in each array should be the latest release, and the
// rest should be in descending order of release date.
export const releases: Record<Platform, Array<ReleaseRecord>> = {
  mac: [
    {
      version: "1.1.4",
      date: "2026-09-04",
      description:
        "Installed with Homebrew? The app now knows, and points you at " +
        "brew upgrade rather than a download that would overwrite it behind " +
        "brew's back.",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.1.4%2FDiskSpace-1.1.4.dmg?alt=media&token=0232b334-122c-4fca-bbb8-f4c56764b934",
      sha256:
        "af9063a8c8b376d8dd6d6fc52ad472977315b29f2bfc501a6cfdc01b5c856f8e",
    },
    {
      version: "1.1.3",
      date: "2026-09-04",
      description:
        "The file list holds still while a scan runs, so a folder can be " +
        "opened without the row moving out from under the pointer. A badge in " +
        "the menu bar reports how the scan is getting on.",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.1.3%2FDiskSpace-1.1.3.dmg?alt=media&token=c9dd1f69-f26b-4308-9821-9765e1127459",
      sha256:
        "e03f123183b203a596d3511d280d313d0baf351a8f4f8e6e798e664fa7482a5a",
    },
    {
      version: "1.1.2",
      date: "2026-09-04",
      description:
        "Fixes a crash on launch: 1.1.1 could not find its own images on any " +
        "Mac but the one that built it, and stopped before opening a window.",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.1.2%2FDiskSpace-1.1.2.dmg?alt=media&token=4ba72c5d-c933-4fe2-970a-1f97ed1ec1c3",
      sha256:
        "ae69f914673bdea6c9f058aa11eb6383c38d2b232972edb341807b91044e3cb3",
    },
    {
      version: "1.1.1",
      date: "2026-09-02",
      description: "",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.1.1%2FDiskSpace-1.1.1.dmg?alt=media&token=727188d8-6db4-440d-a6d9-ebee80c12eed",
    },
    {
      version: "1.0.4",
      date: "2026-08-26",
      description: "First public release of Disk Space for Mac.",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.0.4%2FDiskSpace-1.0.4.dmg?alt=media&token=6f366753-3c3f-4406-8598-ccef0761e144",
    },
  ],
  windows: [
    {
      version: "1.1.4",
      date: "2026-09-04",
      description:
        "Deleting something now holds it for ten seconds in a Pending Deletes " +
        "pane, with a countdown and an Undelete button, so a mistake can be " +
        "taken back. Rows can be selected several at a time and deleted " +
        "together. The file list holds still while a scan runs.",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.1.4%2FDiskSpace.exe?alt=media&token=3b9e4612-e3df-44cf-a3b1-f8104a69f5cd",
      sha256:
        "63615923c02d4c372335942b3996e363438d3ba253365c611623d5d8a5f21617",
      storeUrl: "https://apps.microsoft.com/detail/9nwpgvh9rp8l",
    },
    {
      version: "1.1.1",
      date: "2026-09-02",
      description: "",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.1.1%2FDiskSpace-1.1.1.0.exe?alt=media&token=d4143d6d-d697-4754-913e-ccd36ff22977",
      sha256:
        "36c4d52e2d847031a10531ec1883ade9a531c9e7f929cd2eec880ea458d9c81b",
      storeUrl: "https://apps.microsoft.com/detail/9nwpgvh9rp8l",
    },
    {
      version: "1.1.0",
      date: "2026-08-26",
      description:
        "First public release for Windows, where the app is called Disk Space ++. " +
        "Portable: one file, nothing to install, and no runtime to install either.",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.0.4%2FDiskSpace-1.1.0.0.exe?alt=media&token=b8419e55-098f-42b9-8e6b-698aba5a0137",
      sha256:
        "d87c3a841bfb71a21e9872bddaa9ba7546ba1e9cd2edc1945a3e932fe359ead5",
      storeUrl: "https://apps.microsoft.com/detail/9nwpgvh9rp8l",
    },
  ],
  linux: [
    {
      version: "1.1.4",
      date: "2026-09-04",
      description:
        "Deleting something now holds it for ten seconds in a Pending Deletes " +
        "pane, with a countdown and an Undelete button, so a mistake can be " +
        "taken back. Rows can be selected several at a time and deleted " +
        "together. The file list holds still while a scan runs.",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.1.4%2FDiskSpace-1.1.4-amd64.deb?alt=media&token=1b96a3b2-6527-454c-9af5-26d70be9a218",
      sha256:
        "c7ac6440eeae4c0aeacfd1f31c24ac6983f25831235fa94f65335291af6c49e2",
      alternatives: [
        {
          format: "tarball",
          url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.1.4%2FDiskSpace-1.1.4-x86_64.tar.gz?alt=media&token=0aeee950-bb80-467d-ab87-f8e0de7549df",
          sha256:
            "aeb45b98d07882188fe0bc9c3c3e8a782ee4546951facfb45a45cdf3ea22aff0",
        },
        {
          format: "appimage",
          url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.1.4%2FDiskSpace.AppImage?alt=media&token=296bb17a-8d83-43b2-b54d-075d793e0f30",
          sha256:
            "b531a31c0405321bf5a56e62c0f22cffc3a35aa465422397fd496d38df538582",
        },
      ],
    },
    {
      version: "1.1.1",
      date: "2026-09-02",
      description: "First public release for Linux",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.1.1%2FDiskSpace.AppImage?alt=media&token=2acb7a36-d3b8-4730-8d19-5e949ae6cd5c",
      sha256:
        "4e0b3704c5d65e30deb44282ded3a8e6d71be76ca2dfe4a8dc6fcb27199b498c",
    },
    {
      version: "1.0.0",
      date: "2026-09-02",
      description: "First public release for Linux",
      url: "https://firebasestorage.googleapis.com/v0/b/myelevation-1.firebasestorage.app/o/diskspace%2Freleases%2F1.0.0%2FDiskSpace.AppImage?alt=media&token=674cdab2-db37-4290-be92-a348fbe2e471",
      sha256:
        "6c011b0c098aee522616cd32b306f4727f2db6291551062ace68ffe51fd4cc96",
    },
  ],
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
/// How the Mac app is installed through Homebrew.
///
/// The trust line is not optional: Homebrew refuses to load a cask from a tap
/// outside its own repositories until the tap is trusted, so leaving it out
/// gives everyone an error rather than an install. The cask is disk-space, not
/// diskspace, because diskspace is already taken in the main cask repository by
/// an unrelated tool and a tap cannot outrank it.
export const homebrew = {
  tap: "chofter/tap",
  cask: "disk-space",
  commands: [
    "brew tap chofter/tap",
    "brew trust chofter/tap",
    "brew install --cask disk-space",
  ] as const,
};

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

/// The file name a download url points at - "DiskSpace-1.1.4-amd64.deb" - which
/// is what someone types after downloading it, so the instructions quote it.
export function fileName(url: string): string {
  const path = decodeURIComponent(new URL(url).pathname);
  return path.slice(path.lastIndexOf("/") + 1);
}

/// A short label for a download, from its extension: ".deb", ".tar.gz".
export function fileLabel(url: string): string {
  const name = fileName(url);
  if (name.endsWith(".tar.gz")) {
    return ".tar.gz";
  }
  if (name.endsWith(".AppImage")) {
    return "AppImage";
  }
  const dot = name.lastIndexOf(".");
  return dot >= 0 ? name.slice(dot) : name;
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
