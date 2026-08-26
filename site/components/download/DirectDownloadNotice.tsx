import styles from "./DirectDownloadNotice.module.css";

/// What the Windows executable asks for once someone runs it.
///
/// The file is unsigned, so SmartScreen stops it the first time, and the app
/// asks for administrator rights before scanning a whole drive. Neither is a
/// sign of anything wrong, but both look like one arriving unannounced, and
/// someone who backs out of the SmartScreen dialog has no reason to think the
/// download was fine.
export default function DirectDownloadNotice() {
  return (
    <div className={styles.panel} role="status">
      <p className={styles.title}>Two prompts to expect</p>
      <p className={styles.item}>
        <strong>&ldquo;Windows protected your PC&rdquo;</strong> the first time
        you run it. The download carries no code-signing certificate, so
        SmartScreen stops anything it has not seen before. Choose{" "}
        <em>More info</em>, then <em>Run anyway</em>. The{" "}
        <a href="/releases">releases page</a> lists the file&rsquo;s SHA-256 if
        you would rather check it first.
      </p>
      <p className={styles.item}>
        <strong>A request for administrator rights</strong>, but only when you
        scan a whole drive. Reading the drive&rsquo;s own file table is what
        makes that fast and complete. Declining is fine: the scan still runs the
        ordinary way, slower, and skips folders your account cannot read.
      </p>
    </div>
  );
}
