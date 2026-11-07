"use client";

import CopyButton from "./CopyButton";
import styles from "./CommandBox.module.css";

/// Shell commands in a dark box, one per line, with a button that copies them
/// all. `onCopied` is there for analytics: copying is usually the moment
/// someone commits to an install, since nobody types these out.
export default function CommandBox({
  commands,
  onCopied,
  label = "Copy commands",
}: {
  commands: ReadonlyArray<string>;
  onCopied?: () => void;
  label?: string;
}) {
  return (
    <div className={styles.box}>
      <pre className={styles.code}>
        {commands.map(line => (
          <code key={line} className={styles.line}>
            <span className={styles.prompt} aria-hidden="true">
              $
            </span>
            {line}
          </code>
        ))}
      </pre>
      <CopyButton
        text={commands.join("\n")}
        label={label}
        onCopied={onCopied}
      />
    </div>
  );
}
