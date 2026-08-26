import AppleIcon from "@/components/icons/AppleIcon";
import WindowsIcon from "@/components/icons/WindowsIcon";
import LinuxIcon from "@/components/icons/LinuxIcon";
import {
  latestDirectDownload,
  latestRelease,
  latestStoreListing,
  platformNames,
  type Platform,
} from "@/lib/releases";
import styles from "./DownloadButton.module.css";

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

  // A platform with no releases yet still gets a button, greyed out, so the
  // set of supported platforms reads the same before and after each ships.
  if (!latest || (!download && !store)) {
    return (
      <div className={styles.wrapper}>
        <span className={`${styles.button} ${styles.disabled}`}>
          <Icon />
          {label}
        </span>
        <span className={styles.comingSoon}>Coming soon</span>
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
          rel="noopener noreferrer">
          <Icon />
          {label}
        </a>
        {download ? (
          <a href={download.url} className={styles.secondary} download>
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
      <a href={download!.url} className={styles.button} download>
        <Icon />
        {label}
      </a>
    </div>
  );
}
