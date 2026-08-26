"use client";

import {useEffect, useRef} from "react";
import styles from "@/app/page.module.css";

const videoId = "pmoTzIdnY5E";

// Muted from the start: browsers only allow a video to play by itself when it
// makes no sound, and this one has none to make. Autoplay is added when the
// frame comes into view, not before.
const base =
  `https://www.youtube-nocookie.com/embed/${videoId}` +
  "?mute=1&playsinline=1&rel=0";

export default function DemoVideo() {
  const frame = useRef<HTMLIFrameElement>(null);

  useEffect(() => {
    const iframe = frame.current;
    if (!iframe) {
      return;
    }
    // Starting a video by itself is motion the viewer did not ask for, so it
    // stays a still frame with a play button for anyone who has said they want
    // less of that.
    if (window.matchMedia?.("(prefers-reduced-motion: reduce)").matches) {
      return;
    }
    if (!("IntersectionObserver" in window)) {
      return;
    }

    let started = false;
    const observer = new IntersectionObserver(
      entries => {
        for (const entry of entries) {
          if (!entry.isIntersecting || started) {
            continue;
          }
          started = true;
          // Reloading the player with autoplay is enough — it has not played
          // yet, so nothing is interrupted.
          iframe.src = `${base}&autoplay=1`;
          observer.disconnect();
        }
      },
      // Half the frame on screen: enough to be looking at it, rather than
      // firing while it is still a sliver at the edge.
      {threshold: 0.5},
    );

    observer.observe(iframe);
    return () => observer.disconnect();
  }, []);

  return (
    <iframe
      ref={frame}
      className={styles.video}
      src={base}
      title="Disk Space in action"
      loading="lazy"
      allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share"
      referrerPolicy="strict-origin-when-cross-origin"
      allowFullScreen
    />
  );
}
