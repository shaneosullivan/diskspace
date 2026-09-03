"use client";

import {homebrew} from "@/lib/releases";
import {trackClient} from "@/lib/track-client";
import CopyButton from "./CopyButton";
import styles from "./HomebrewBox.module.css";

/// The three Homebrew commands, with a copy button on the box.
///
/// Copying is the action worth counting here — nobody types these out — so it is
/// what reports the install, under the same event as a download with its own via
/// so the Homebrew share is legible next to the DMG one.
export default function HomebrewBox({version}: {version: string}) {
  const script = homebrew.commands.join("\n");

  return (
    <div className={styles.box}>
      <pre className={styles.code}>
        {homebrew.commands.map(line => (
          <code key={line} className={styles.line}>
            <span className={styles.prompt} aria-hidden="true">
              $
            </span>
            {line}
          </code>
        ))}
      </pre>
      <CopyButton
        text={script}
        label="Copy commands"
        onCopied={() =>
          trackClient("download", {platform: "macos", version, via: "homebrew"})
        }
      />
    </div>
  );
}
