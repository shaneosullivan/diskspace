"use client";

import AppleIcon from "@/components/icons/AppleIcon";
import WindowsIcon from "@/components/icons/WindowsIcon";
import LinuxIcon from "@/components/icons/LinuxIcon";
import {
  latestRelease,
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
  const Icon = icons[platform];
  const label = platformNames[platform];

  // A platform with no releases yet still gets a button, greyed out, so the
  // set of supported platforms reads the same before and after each ships.
  if (!latest) {
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

  return (
    <div className={styles.wrapper}>
      <a
        href={latest.url}
        className={styles.button}
        download
        onClick={() =>
          trackClient("download", {
            // The same name the app reports, so a download and a later update
            // check from that install line up.
            platform: runtimeNames[platform],
            version: latest.version,
          })
        }>
        <Icon />
        {label}
      </a>
      {/* <span className={styles.version}>Version {latest.version}</span> */}
    </div>
  );
}
