import Link from "next/link";
import DownloadNavLink from "@/components/download/DownloadNavLink";
import styles from "./Footer.module.css";

export default function Footer() {
  return (
    <footer className={styles.footer}>
      <div className="container">
        <div className={styles.row}>
          <span className={styles.brand}>Disk Space</span>
          <nav className={styles.links}>
            <Link href="/#features">Features</Link>
            <DownloadNavLink location="footer">Download</DownloadNavLink>
            <Link href="/#faq">Questions</Link>
            <Link href="/privacy">Privacy</Link>
            <Link href="/releases">Releases</Link>
          </nav>
        </div>
        <p className={styles.note}>
          Everything the app measures stays on your machine. Nothing is
          uploaded.
        </p>
      </div>
    </footer>
  );
}
