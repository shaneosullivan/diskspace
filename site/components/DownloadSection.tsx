"use client";

import {useState} from "react";

import DirectDownloadNotice from "@/components/download/DirectDownloadNotice";
import DownloadButton from "@/components/download/DownloadButton";
import styles from "./DownloadSection.module.css";

export default function DownloadSection() {
  // Below the whole row rather than under the button that was pressed, so the
  // buttons stay exactly where they were when it appears.
  const [explainWindows, setExplainWindows] = useState(false);

  return (
    <div>
      <div className={styles.buttons}>
        <DownloadButton platform="mac" />
        <DownloadButton
          platform="windows"
          onDirectDownload={() => setExplainWindows(true)}
        />
        <DownloadButton platform="linux" />
      </div>
      {explainWindows ? <DirectDownloadNotice /> : null}
    </div>
  );
}
