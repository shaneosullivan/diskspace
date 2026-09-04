"use client";

import {useEffect, useRef, useState} from "react";

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

// A link can arrive pointed at one platform — /#download-macos and its two
// siblings — and open that platform's options as though the button had been
// pressed. Only the row at the top of the page answers to these, so a link
// cannot open two panels at once and set them scrolling against each other.
const platformHashes: Record<string, Platform> = {
  "#download-macos": "mac",
  "#download-windows": "windows",
  "#download-linux": "linux",
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
export default function DownloadSection({
  openFromHash = false,
}: {
  /// Whether a #download-<platform> url should open a platform here.
  openFromHash?: boolean;
}) {
  const [selected, setSelected] = useState<Platform | null>(null);
  const rowRef = useRef<HTMLDivElement>(null);

  // Shortly after the page settles rather than on the first frame: opening the
  // panel makes the row scroll itself into view, and that measures the header,
  // which is still finding its height while fonts load.
  useEffect(() => {
    if (!openFromHash) {
      return;
    }
    const platform = platformHashes[window.location.hash];
    if (!platform || !isReleased(platform)) {
      return;
    }
    const timer = window.setTimeout(() => setSelected(platform), 250);
    return () => window.clearTimeout(timer);
  }, [openFromHash]);

  // On a phone the options open below the fold, and the button that was tapped
  // is often near the bottom of the screen — so the panel lands out of sight.
  // Bring the row up to just under the header, which puts all three buttons and
  // the panel they reveal on screen together. Only on a narrow screen: on a
  // wide one the panel already fits and a jump would be jarring.
  useEffect(() => {
    if (!selected) {
      return;
    }
    if (!window.matchMedia("(max-width: 640px)").matches) {
      return;
    }
    const row = rowRef.current;
    if (!row) {
      return;
    }
    const header = document.querySelector("header");
    const offset = (header?.offsetHeight ?? 0) + 12;
    const top = row.getBoundingClientRect().top + window.scrollY - offset;
    window.scrollTo({
      top,
      behavior: window.matchMedia("(prefers-reduced-motion: reduce)").matches
        ? "auto"
        : "smooth",
    });
  }, [selected]);

  return (
    <div className={styles.section}>
      <div ref={rowRef} className={styles.buttons}>
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
