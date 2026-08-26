import {faq} from "@/lib/faq";
import {
  latestRelease,
  releases,
  runtimeNames,
  type Platform,
} from "@/lib/releases";
import {siteUrl} from "@/lib/site";

/// Machine-readable descriptions of the app and the answers below it.
///
/// The application entry is what a search engine needs to show this as software
/// rather than as an article — the platform it runs on, that it costs nothing,
/// and where the current build is. The questions come from the same list the
/// page renders, so the two can never disagree.
export default function StructuredData() {
  const mac = latestRelease("mac");
  const platforms = (Object.keys(releases) as Array<Platform>)
    .filter(p => releases[p].length > 0)
    .map(p => runtimeNames[p]);

  const application = {
    "@context": "https://schema.org",
    "@type": "SoftwareApplication",
    name: "Disk Space",
    applicationCategory: "UtilitiesApplication",
    applicationSubCategory: "Disk usage analyzer",
    operatingSystem: platforms.includes("macos")
      ? "macOS 15 or later"
      : undefined,
    description:
      "A free disk usage analyser for macOS. Scans a folder or a whole volume in parallel, drawing every file in proportion to its size, and shows what arrived recently as well as what is large.",
    url: siteUrl,
    downloadUrl: mac?.url,
    softwareVersion: mac?.version,
    datePublished: mac?.date,
    screenshot: `${siteUrl}/screenshot.jpg`,
    isAccessibleForFree: true,
    offers: {
      "@type": "Offer",
      price: "0",
      priceCurrency: "USD",
    },
    publisher: {
      "@type": "Organization",
      name: "Chofter Apps",
      url: "https://www.chofter.com",
    },
  };

  const questions = {
    "@context": "https://schema.org",
    "@type": "FAQPage",
    mainEntity: faq.map(entry => ({
      "@type": "Question",
      name: entry.question,
      acceptedAnswer: {"@type": "Answer", text: entry.answer},
    })),
  };

  return (
    <>
      <script
        type="application/ld+json"
        dangerouslySetInnerHTML={{__html: JSON.stringify(application)}}
      />
      <script
        type="application/ld+json"
        dangerouslySetInnerHTML={{__html: JSON.stringify(questions)}}
      />
    </>
  );
}
