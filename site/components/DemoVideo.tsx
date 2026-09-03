"use client";

import {useEffect, useRef} from "react";
import styles from "@/app/page.module.css";

const videoId = "pmoTzIdnY5E";

// Muted (so it may play by itself), inline (so iOS Safari does not go
// fullscreen), and with the JS API on, so it can be told to play once it is on
// screen — without reloading it, which is the part the old version got wrong.
const src =
  `https://www.youtube-nocookie.com/embed/${videoId}` +
  "?mute=1&playsinline=1&rel=0&enablejsapi=1";

/// The demo video.
///
/// The player is loaded once, straight away, and shown as its poster until it is
/// scrolled into view, at which point it is asked to play through the YouTube
/// API. The old version instead swapped the iframe's src to add autoplay, which
/// reloaded the player; on a real phone that second load raced the first and
/// sometimes left a blank white frame. Loading once and sending a play command
/// avoids the reload entirely, and the frame behind it is dark, so nothing shows
/// white even if the player is slow.
export default function DemoVideo() {
  const ref = useRef<HTMLIFrameElement>(null);

  useEffect(() => {
    const iframe = ref.current;
    if (!iframe) {
      return;
    }
    // Someone who asked for less motion keeps the poster and its play button,
    // and nothing starts on its own.
    if (window.matchMedia?.("(prefers-reduced-motion: reduce)").matches) {
      return;
    }

    let inView = false;
    let loaded = false;
    let started = false;

    const play = () => {
      if (!inView || !loaded || started) {
        return;
      }
      started = true;
      const command = JSON.stringify({
        event: "command",
        func: "playVideo",
        args: [],
      });
      // A couple of times over the first second: the player accepts commands
      // only once its own script is ready, which is a moment after the frame's
      // load event, and a dropped first command would otherwise leave it paused.
      iframe.contentWindow?.postMessage(command, "*");
      window.setTimeout(
        () => iframe.contentWindow?.postMessage(command, "*"),
        400,
      );
      window.setTimeout(
        () => iframe.contentWindow?.postMessage(command, "*"),
        900,
      );
    };

    const onLoad = () => {
      loaded = true;
      play();
    };
    iframe.addEventListener("load", onLoad);

    let observer: IntersectionObserver | undefined;
    if ("IntersectionObserver" in window) {
      observer = new IntersectionObserver(
        entries => {
          for (const entry of entries) {
            if (entry.isIntersecting) {
              inView = true;
              play();
              observer?.disconnect();
              break;
            }
          }
        },
        // Half the frame on screen, rather than a sliver at the edge.
        {threshold: 0.5},
      );
      observer.observe(iframe);
    } else {
      // No way to tell when it scrolls into view, so let it play once loaded.
      inView = true;
      play();
    }

    return () => {
      iframe.removeEventListener("load", onLoad);
      observer?.disconnect();
    };
  }, []);

  return (
    <iframe
      ref={ref}
      className={styles.video}
      src={src}
      title="Disk Space in action"
      allow="autoplay; encrypted-media; picture-in-picture; web-share"
      referrerPolicy="strict-origin-when-cross-origin"
      allowFullScreen
    />
  );
}
