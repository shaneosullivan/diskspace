"use client";

import AppleIcon from "@/components/icons/AppleIcon";
import WindowsIcon from "@/components/icons/WindowsIcon";
import LinuxIcon from "@/components/icons/LinuxIcon";
import {
  latestDirectDownload,
  latestRelease,
  latestStoreListing,
  platformNames,
  runtimeNames,
  type Platform,
} from "@/lib/releases";
import {trackClient} from "@/lib/track-client";
import styles from "./DownloadButton.module.css";

// What a platform with nothing to download yet says of itself. Only a platform
// whose situation differs from "not started" needs naming here: Windows is
// built and sitting with the Microsoft Store.
const waitingDefault = "Coming soon";
const waitingLabels: Partial<Record<Platform, string>> = {
  windows: "Pending review",
};

const icons = {
  mac: AppleIcon,
  windows: WindowsIcon,
  linux: LinuxIcon,
};

interface Props {
  platform: Platform;
}

export default function DownloadButton({platform}: Props) {
  const latest = latestRelease(platform);
  const download = latestDirectDownload(platform);
  const store = latestStoreListing(platform);
  const Icon = icons[platform];
  const label = platformNames[platform];

  // A click can now leave for either of two places, so which one it was is part
  // of the event. The platform name is the one the app reports for itself, so a
  // download and a later update check from that install line up.
  const report = (via: "direct" | "store", version: string) =>
    trackClient("download", {platform: runtimeNames[platform], version, via});

  // A platform with no releases yet still gets a button, greyed out, so the
  // set of supported platforms reads the same before and after each ships.
  if (!latest || (!download && !store)) {
    return (
      <div className={styles.wrapper}>
        <span className={`${styles.button} ${styles.disabled}`}>
          <Icon />
          {label}
        </span>
        <span className={styles.comingSoon}>
          {waitingLabels[platform] ?? waitingDefault}
        </span>
      </div>
    );
  }

  // The store is the better first choice where there is one: it is signed, so it
  // installs without the warnings an unsigned download collects, and it updates
  // itself. The direct download stays available underneath for anyone who would
  // rather not use it.
  if (store) {
    return (
      <div className={styles.wrapper}>
        <a
          href={store.storeUrl}
          className={styles.button}
          target="_blank"
          rel="noopener noreferrer"
          onClick={() => report("store", store.version)}>
          <Icon />
          {label}
        </a>
        {download ? (
          <a
            href={download.url}
            className={styles.secondary}
            download
            onClick={() => report("direct", download.version)}>
            or download {download.version} directly
          </a>
        ) : (
          <span className={styles.version}>Microsoft Store</span>
        )}
      </div>
    );
  }

  return (
    <div className={styles.wrapper}>
      <a
        href={download!.url}
        className={styles.button}
        download
        onClick={() => report("direct", download!.version)}>
        <Icon />
        {label}
      </a>
    </div>
  );
}
