"use client";

import {homebrew} from "@/lib/releases";
import {trackClient} from "@/lib/track-client";
import CommandBox from "./CommandBox";

/// The three Homebrew commands, with a copy button on the box.
///
/// Copying is the action worth counting here — nobody types these out — so it is
/// what reports the install, under the same event as a download with its own via
/// so the Homebrew share is legible next to the DMG one.
export default function HomebrewBox({version}: {version: string}) {
  return (
    <CommandBox
      commands={homebrew.commands}
      onCopied={() =>
        trackClient("download", {platform: "macos", version, via: "homebrew"})
      }
    />
  );
}
