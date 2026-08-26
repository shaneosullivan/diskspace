"use client";

import Link from "next/link";
import type {ReactNode} from "react";

import {trackClient} from "@/lib/track-client";

interface Props {
  /// Which of the two links this is, so the funnel can tell them apart.
  location: "header" | "footer";
  className?: string;
  children: ReactNode;
}

/// A link to the download section that records the click.
///
/// It reports "download-nav" rather than "download": nothing is downloaded by
/// pressing it, and counting it as a download would inflate the number that
/// matters.
export default function DownloadNavLink({
  location,
  className,
  children,
}: Props) {
  return (
    <Link
      href="/#download"
      className={className}
      onClick={() => trackClient("download-nav", {location})}>
      {children}
    </Link>
  );
}
