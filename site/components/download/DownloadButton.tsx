import AppleIcon from "@/components/icons/AppleIcon";
import WindowsIcon from "@/components/icons/WindowsIcon";
import LinuxIcon from "@/components/icons/LinuxIcon";
import {latestRelease, platformNames, type Platform} from "@/lib/releases";
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
        <span className={styles.comingSoon}>Coming soon</span>
      </div>
    );
  }

  return (
    <div className={styles.wrapper}>
      <a href={latest.url} className={styles.button} download>
        <Icon />
        {label}
      </a>
      {/* <span className={styles.version}>Version {latest.version}</span> */}
    </div>
  );
}
