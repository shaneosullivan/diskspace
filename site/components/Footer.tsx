import Link from "next/link";
import styles from "./Footer.module.css";

export default function Footer() {
  return (
    <footer className={styles.footer}>
      <div className="container">
        <div className={styles.row}>
          <span className={styles.brand}>Disk Space</span>
          <nav className={styles.links}>
            <Link href="/#features">Features</Link>
            <Link href="/#download">Download</Link>
            <Link href="/releases">Releases</Link>
            <a href="https://github.com/shaneosullivan/diskspace">Source</a>
          </nav>
        </div>
        <p className={styles.note}>
          Everything the app measures stays on your machine. Nothing is uploaded
          — the app&apos;s one network request is the update check when you open
          About.
        </p>
      </div>
    </footer>
  );
}
