import DownloadButton from "@/components/download/DownloadButton";
import styles from "./DownloadSection.module.css";

export default function DownloadSection() {
  return (
    <div className={styles.buttons}>
      <DownloadButton platform="mac" />
      <DownloadButton platform="windows" />
      <DownloadButton platform="linux" />
    </div>
  );
}
