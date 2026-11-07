"use client";

import {
  fileName,
  latestDirectDownload,
  type AlternativeDownload,
} from "@/lib/releases";
import {trackClient} from "@/lib/track-client";
import CommandBox from "./CommandBox";
import styles from "./LinuxDownloads.module.css";

type Via = "deb" | AlternativeDownload["format"];

type Option = {
  via: Via;
  title: string;
  badge?: string;
  body: string;
  url: string;
  sha256?: string;
  commands: Array<string>;
  note: React.ReactNode;
};

/// The three Linux formats, each with the commands that install or run it.
///
/// The .deb comes first and is the one to recommend: the package manager pulls
/// in the five libraries the app links, and it lands in the applications menu.
/// The AppImage comes last, because it mounts itself through FUSE 2, which
/// Ubuntu has left out of a default install since 22.04.
export default function LinuxDownloads() {
  const release = latestDirectDownload("linux");
  if (!release?.url) {
    return null;
  }
  const version = release.version;
  const alternative = (format: AlternativeDownload["format"]) =>
    release.alternatives?.find(a => a.format === format);

  const options: Array<Option> = [];

  const deb = fileName(release.url);
  options.push({
    via: "deb",
    title: ".deb package",
    badge: "Recommended",
    body: "For Ubuntu, Debian, Mint, Pop!_OS and other apt-based systems. Installs everything it needs and adds Disk Space to your applications menu.",
    url: release.url,
    sha256: release.sha256,
    commands: ["cd ~/Downloads", `sudo apt install ./${deb}`],
    note: (
      <>
        Keep the <code>./</code>, which tells apt this is a file rather than a
        package name. Launch it from the menu, or run <code>diskspace</code>. To
        remove it: <code>sudo apt remove disk-space</code>.
      </>
    ),
  });

  const tarball = alternative("tarball");
  if (tarball) {
    const name = fileName(tarball.url);
    options.push({
      via: "tarball",
      title: "Tarball",
      body: "For any distribution. Nothing to install: unpack it and run it from wherever you like.",
      url: tarball.url,
      sha256: tarball.sha256,
      commands: [
        "cd ~/Downloads",
        `tar xzf ${name}`,
        "./com.chofter.DiskSpace.AppDir/usr/bin/diskspace",
      ],
      note: (
        <>
          Needs libX11, libXft, libXrender, fontconfig and libpng, which every X
          desktop already has.
        </>
      ),
    });
  }

  const appImage = alternative("appimage");
  if (appImage) {
    const name = fileName(appImage.url);
    options.push({
      via: "appimage",
      title: "AppImage",
      body: "A single portable file. Make it executable and run it.",
      url: appImage.url,
      sha256: appImage.sha256,
      commands: ["cd ~/Downloads", `chmod +x ${name}`, `./${name}`],
      note: (
        <>
          Needs FUSE 2, which Ubuntu 22.04 and later leave out. Install it with{" "}
          <code>sudo apt install libfuse2t64</code> (<code>libfuse2</code> on
          22.04), or use the .deb instead.
        </>
      ),
    });
  }

  const report = (via: Via) =>
    trackClient("download", {platform: "linux", version, via});

  return (
    <div className={styles.options}>
      {options.map((option, index) => (
        <section key={option.via} className={styles.card}>
          <div className={styles.head}>
            <h2 className={styles.title}>{option.title}</h2>
            {option.badge ? (
              <span className={styles.badge}>{option.badge}</span>
            ) : null}
          </div>
          <p className={styles.body}>{option.body}</p>
          <a
            href={option.url}
            className={index === 0 ? styles.primary : styles.secondary}
            download
            onClick={() => report(option.via)}>
            Download {fileName(option.url)}
          </a>
          <div className={styles.commands}>
            <CommandBox
              commands={option.commands}
              onCopied={() =>
                trackClient("download-copy", {
                  platform: "linux",
                  version,
                  via: option.via,
                })
              }
            />
          </div>
          <p className={styles.note}>{option.note}</p>
          {option.sha256 ? (
            <p className={styles.checksum}>
              <span className={styles.checksumLabel}>SHA-256</span>
              <code>{option.sha256}</code>
            </p>
          ) : null}
        </section>
      ))}
    </div>
  );
}
