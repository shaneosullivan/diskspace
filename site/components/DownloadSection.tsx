"use client";

import {useState} from "react";

import AppleIcon from "@/components/icons/AppleIcon";
import LinuxIcon from "@/components/icons/LinuxIcon";
import WindowsIcon from "@/components/icons/WindowsIcon";
import DownloadOptions from "@/components/download/DownloadOptions";
import {
  latestDirectDownload,
  latestStoreListing,
  platformNames,
  type Platform,
} from "@/lib/releases";
import styles from "./DownloadSection.module.css";

const order: Array<Platform> = ["mac", "windows", "linux"];

const icons: Record<Platform, () => React.JSX.Element> = {
  mac: AppleIcon,
  windows: WindowsIcon,
  linux: LinuxIcon,
};

// What a platform with nothing to download yet says of itself. Only a platform
// whose situation differs from "not started" needs naming here.
const waitingDefault = "Coming soon";
const waitingLabels: Partial<Record<Platform, string>> = {
  windows: "Pending review",
};

function isReleased(platform: Platform): boolean {
  return (
    Boolean(latestDirectDownload(platform)) ||
    Boolean(latestStoreListing(platform))
  );
}

/// The three platforms as a row of buttons that reveal their download options
/// below, rather than each carrying its own link.
///
/// A platform can offer more than one way in — a disk image or Homebrew on the
/// Mac, the Store or a direct download on Windows — and those do not fit on a
/// button. Selecting a platform opens them underneath, so the row stays a row
/// and the choice has room to explain itself.
export default function DownloadSection() {
  const [selected, setSelected] = useState<Platform | null>(null);

  return (
    <div className={styles.section}>
      <div className={styles.buttons}>
        {order.map(platform => {
          const Icon = icons[platform];
          const released = isReleased(platform);
          const active = selected === platform;

          if (!released) {
            return (
              <div key={platform} className={styles.wrapper}>
                <span className={`${styles.button} ${styles.disabled}`}>
                  <Icon />
                  {platformNames[platform]}
                </span>
                <span className={styles.caption}>
                  {waitingLabels[platform] ?? waitingDefault}
                </span>
              </div>
            );
          }

          return (
            <div key={platform} className={styles.wrapper}>
              <button
                type="button"
                className={`${styles.button} ${
                  active ? styles.outlined : styles.filled
                }`}
                aria-expanded={active}
                onClick={() => setSelected(active ? null : platform)}>
                <Icon />
                {platformNames[platform]}
                <svg
                  className={`${styles.chevron} ${active ? styles.chevronUp : ""}`}
                  width="12"
                  height="12"
                  viewBox="0 0 12 12"
                  aria-hidden="true">
                  <path
                    d="M2.5 4.5 6 8l3.5-3.5"
                    fill="none"
                    stroke="currentColor"
                    strokeWidth="1.5"
                    strokeLinecap="round"
                    strokeLinejoin="round"
                  />
                </svg>
              </button>
            </div>
          );
        })}
      </div>

      {selected ? <DownloadOptions platform={selected} /> : null}
    </div>
  );
}
