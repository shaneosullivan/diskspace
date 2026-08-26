import type {MetadataRoute} from "next";

import {releases} from "@/lib/releases";
import {siteUrl} from "@/lib/site";

export default function sitemap(): MetadataRoute.Sitemap {
  // The newest release is the last time either page said anything new.
  const newest = Object.values(releases)
    .flat()
    .map(r => r.date)
    .sort()
    .at(-1);
  const lastModified = newest ? new Date(`${newest}T00:00:00Z`) : new Date();

  return [
    {
      url: `${siteUrl}/`,
      lastModified,
      changeFrequency: "monthly",
      priority: 1,
    },
    {
      url: `${siteUrl}/releases`,
      lastModified,
      changeFrequency: "monthly",
      priority: 0.6,
    },
  ];
}
