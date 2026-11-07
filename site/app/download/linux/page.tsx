import type {Metadata} from "next";

import Footer from "@/components/Footer";
import Header from "@/components/Header";
import LinuxDownloads from "@/components/download/LinuxDownloads";
import {formatDate, latestDirectDownload} from "@/lib/releases";
import {linuxDownloadPath, siteUrl} from "@/lib/site";
import styles from "./page.module.css";

const description =
  "Download Disk Space for Linux as a .deb package, a tarball or an AppImage, with the commands to install and run each one.";

export const metadata: Metadata = {
  title: "Download for Linux",
  description,
  alternates: {canonical: linuxDownloadPath},
  openGraph: {
    title: "Download Disk Space for Linux",
    description,
    url: `${siteUrl}${linuxDownloadPath}`,
    // Declaring openGraph here replaces the layout's, image included.
    images: ["/og-image.jpg"],
    type: "website",
  },
};

export default function LinuxDownload() {
  const release = latestDirectDownload("linux");

  return (
    <>
      <Header />
      <main className={`container ${styles.main}`}>
        <h1 className={styles.title}>Download for Linux</h1>
        {release ? (
          <p className={styles.lead}>
            Version {release.version}, released {formatDate(release.date)}. The
            same build in three formats, all for x86-64. If you are not sure
            which to pick, take the .deb.
          </p>
        ) : null}
        <LinuxDownloads />
        <p className={styles.more}>
          Older versions and what changed in each are on the{" "}
          <a href="/releases">releases page</a>.
        </p>
      </main>
      <Footer />
    </>
  );
}
