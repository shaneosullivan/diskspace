"use client";

import {useEffect, useRef} from "react";
import styles from "@/app/page.module.css";

const videoId = "pmoTzIdnY5E";

// Muted (so it may play by itself), inline (so iOS Safari does not go
// fullscreen). Autoplay is in the URL from the first load, not added later by
// swapping the src — that swap reloaded the player and raced its first load,
// which is what left a blank white frame on a phone.
const base =
  `https://www.youtube-nocookie.com/embed/${videoId}` +
  "?mute=1&playsinline=1&rel=0";
const autoplaySrc = `${base}&autoplay=1`;

/// The demo video, muted and playing itself.
///
/// It loads once, already carrying autoplay, so there is no second load to go
/// wrong. Someone who asked for less motion has the player pointed at the
/// no-autoplay url instead — the one case that reloads, and only to stop a video
/// rather than start one.
export default function DemoVideo() {
  const ref = useRef<HTMLIFrameElement>(null);

  useEffect(() => {
    const iframe = ref.current;
    if (
      iframe &&
      window.matchMedia?.("(prefers-reduced-motion: reduce)").matches
    ) {
      iframe.src = base;
    }
  }, []);

  return (
    <iframe
      ref={ref}
      className={styles.video}
      src={autoplaySrc}
      title="Disk Space in action"
      allow="autoplay; encrypted-media; picture-in-picture; web-share"
      referrerPolicy="strict-origin-when-cross-origin"
      allowFullScreen
    />
  );
}
