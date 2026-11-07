"use client";

import {useState} from "react";

import styles from "./CommandBox.module.css";

/// Copies `text`, then shows a tick for a moment so the click has an answer.
///
/// Guarded because the clipboard API is absent on an insecure origin and in
/// older browsers; the button then does nothing rather than throwing, and the
/// commands beside it are still selectable by hand.
export default function CopyButton({
  text,
  onCopied,
  label = "Copy",
}: {
  text: string;
  onCopied?: () => void;
  label?: string;
}) {
  const [copied, setCopied] = useState(false);

  const copy = async () => {
    try {
      await navigator.clipboard.writeText(text);
    } catch {
      return;
    }
    setCopied(true);
    onCopied?.();
    window.setTimeout(() => setCopied(false), 2000);
  };

  return (
    <button
      type="button"
      className={styles.copy}
      onClick={copy}
      aria-label={copied ? "Copied" : label}
      title={copied ? "Copied" : label}>
      {copied ? (
        <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true">
          <path
            d="M13.5 4.5 6.5 11.5 3 8"
            fill="none"
            stroke="currentColor"
            strokeWidth="1.6"
            strokeLinecap="round"
            strokeLinejoin="round"
          />
        </svg>
      ) : (
        <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true">
          <rect
            x="5.25"
            y="5.25"
            width="7.5"
            height="7.5"
            rx="1.4"
            fill="none"
            stroke="currentColor"
            strokeWidth="1.3"
          />
          <path
            d="M3.25 10.5V4a1.25 1.25 0 0 1 1.25-1.25H10"
            fill="none"
            stroke="currentColor"
            strokeWidth="1.3"
            strokeLinecap="round"
          />
        </svg>
      )}
    </button>
  );
}
