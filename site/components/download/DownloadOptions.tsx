"use client";

import DirectDownloadNotice from "@/components/download/DirectDownloadNotice";
import HomebrewBox from "@/components/download/HomebrewBox";
import {
  latestDirectDownload,
  latestStoreListing,
  runtimeNames,
  type Platform,
} from "@/lib/releases";
import {trackClient} from "@/lib/track-client";
import styles from "./DownloadOptions.module.css";

/// The ways in for one platform, laid out side by side under the selector.
///
/// Mac: the disk image, and Homebrew. Windows: the Microsoft Store, and the
/// direct download — which is unsigned, so the two prompts it raises are spelled
/// out below the row. Linux has a page of its own instead of a panel.
export default function DownloadOptions({platform}: {platform: Platform}) {
  const download = latestDirectDownload(platform);
  const store = latestStoreListing(platform);

  const report = (via: "direct" | "store", version: string) =>
    trackClient("download", {platform: runtimeNames[platform], version, via});

  if (platform === "mac") {
    return (
      <div className={styles.panel}>
        <div className={styles.cards}>
          {download ? (
            <section className={styles.card}>
              <h3 className={styles.cardTitle}>Download</h3>
              <p className={styles.cardBody}>
                A signed, notarised disk image. Drag it to Applications.
              </p>
              <a
                href={download.url}
                className={styles.primary}
                download
                onClick={() => report("direct", download.version)}>
                Download {download.version}
              </a>
              <p className={styles.meta}>.dmg · requires macOS 15 or later</p>
            </section>
          ) : null}

          <section className={styles.card}>
            <h3 className={styles.cardTitle}>Homebrew</h3>
            <p className={styles.cardBody}>
              Install and update from the command line.
            </p>
            <HomebrewBox version={download?.version ?? ""} />
          </section>
        </div>
      </div>
    );
  }

  if (platform === "windows") {
    return (
      <div className={styles.panel}>
        <div className={styles.cards}>
          {store ? (
            <section className={styles.card}>
              <h3 className={styles.cardTitle}>Microsoft Store</h3>
              <p className={styles.cardBody}>
                Signed, and it keeps itself up to date. The recommended way in.
              </p>
              <a
                href={store.storeUrl}
                className={styles.primary}
                target="_blank"
                rel="noopener noreferrer"
                onClick={() => report("store", store.version)}>
                Get it from the Store
              </a>
              <p className={styles.meta}>Disk Space ++</p>
            </section>
          ) : null}

          {download ? (
            <section className={styles.card}>
              <h3 className={styles.cardTitle}>Direct download</h3>
              <p className={styles.cardBody}>
                A single portable file. Nothing to install, no runtime needed.
              </p>
              <a
                href={download.url}
                className={styles.secondary}
                download
                onClick={() => report("direct", download.version)}>
                Download {download.version}
              </a>
              <p className={styles.meta}>.exe · Windows 10 or later</p>
            </section>
          ) : null}
        </div>
        {download ? <DirectDownloadNotice /> : null}
      </div>
    );
  }

  // Linux has a page of its own, /download/linux, which the button links to.
  return null;
}
