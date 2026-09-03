import type {Metadata} from "next";
import Header from "@/components/Header";
import Footer from "@/components/Footer";
import {
  formatDate,
  platformNames,
  releases,
  type Platform,
} from "@/lib/releases";
import {siteUrl} from "@/lib/site";
import styles from "./page.module.css";

export const metadata: Metadata = {
  title: "Releases",
  description:
    "Every published version of Disk Space for Mac and Windows, newest first, with what changed and a link to download it. Older builds stay available.",
  alternates: {canonical: "/releases"},
  openGraph: {
    title: "Disk Space releases",
    description:
      "Every published version of Disk Space for Mac and Windows, newest first, with what changed and a link to download it.",
    url: `${siteUrl}/releases`,
    // Declaring openGraph here replaces the layout's, image included.
    images: ["/og-image.jpg"],
    type: "website",
  },
};

const platforms = Object.keys(releases) as Array<Platform>;

export default function Releases() {
  return (
    <>
      <Header />
      <main className="container">
        <h1 className={styles.title}>Releases</h1>
        <p className={styles.intro}>
          Every version ever published. Older builds stay downloadable.
        </p>

        {platforms.map(platform => {
          const list = releases[platform];
          return (
            <section key={platform} className={styles.platform}>
              <h2 className={styles.platformName}>{platformNames[platform]}</h2>
              {list.length === 0 ? (
                <p className={styles.empty}>Not released yet.</p>
              ) : (
                <ul className={styles.list}>
                  {list.map(release => (
                    <li key={release.version} className={styles.release}>
                      <div className={styles.releaseHead}>
                        <span className={styles.version}>
                          {release.version}
                        </span>
                        <span className={styles.date}>
                          {formatDate(release.date)}
                        </span>
                        {/* Whichever links this release actually has. A store
                            submission can still be in review while the direct
                            download is already up, so neither is assumed. */}
                        {release.storeUrl ? (
                          <a
                            href={release.storeUrl}
                            className={styles.download}
                            target="_blank"
                            rel="noopener noreferrer">
                            Store
                          </a>
                        ) : null}
                        {release.url ? (
                          <a
                            href={release.url}
                            className={styles.download}
                            download>
                            Download
                          </a>
                        ) : null}
                      </div>
                      <p className={styles.description}>
                        {release.description}
                      </p>
                      {release.sha256 ? (
                        <p className={styles.checksum}>
                          <span className={styles.checksumLabel}>SHA-256</span>
                          <code>{release.sha256}</code>
                        </p>
                      ) : null}
                    </li>
                  ))}
                </ul>
              )}
            </section>
          );
        })}
      </main>
      <Footer />
    </>
  );
}
