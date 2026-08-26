import Link from "next/link";
import Image from "next/image";
import DownloadNavLink from "@/components/download/DownloadNavLink";
import styles from "./Header.module.css";

export default function Header() {
  return (
    <header className={styles.header}>
      <div className={styles.container}>
        <Link href="/" className={styles.logo}>
          <Image
            src="/app_icon.png"
            alt=""
            width={32}
            height={32}
            className={styles.logoIcon}
          />
          <span className={styles.logoText}>Disk Space</span>
        </Link>

        <nav className={styles.nav}>
          <Link href="/#features" className={styles.navLink}>
            Features
          </Link>
          <Link href="/#speed" className={styles.navLink}>
            Speed
          </Link>
        </nav>

        <DownloadNavLink location="header" className="btn btn-primary">
          Download
        </DownloadNavLink>
      </div>
    </header>
  );
}
