import {faq} from "@/lib/faq";
import {
  latestRelease,
  platformRequirements,
  releasedPlatforms,
} from "@/lib/releases";
import {linuxDownloadsUrl, siteUrl} from "@/lib/site";

/// Machine-readable descriptions of the app and the answers below it.
///
/// The application entry is what a search engine needs to show this as software
/// rather than as an article — the platform it runs on, that it costs nothing,
/// and where the current build is. The questions come from the same list the
/// page renders, so the two can never disagree.
export default function StructuredData() {
  const publisher = {
    "@type": "Organization",
    name: "Chofter Apps",
    url: "https://www.chofter.com",
  };
  const free = {"@type": "Offer", price: "0", priceCurrency: "USD"};

  // One entry per platform that has shipped, rather than one entry describing
  // three applications. A search for the Windows build should be able to match
  // something that names Windows, carries the Windows version number and links
  // to the Windows download.
  const applications = releasedPlatforms().map(platform => {
    const release = latestRelease(platform);
    return {
      "@context": "https://schema.org",
      "@type": "SoftwareApplication",
      name: platform === "windows" ? "Disk Space ++" : "Disk Space",
      applicationCategory: "UtilitiesApplication",
      applicationSubCategory: "Disk usage analyzer",
      operatingSystem: platformRequirements[platform],
      description:
        "A free disk usage analyser. Scans a folder or a whole drive in parallel, " +
        "draws every file in proportion to its size, and shows what arrived " +
        "recently as well as what is large.",
      url: siteUrl,
      downloadUrl:
        platform === "linux"
          ? linuxDownloadsUrl
          : (release?.storeUrl ?? release?.url ?? `${siteUrl}/#download`),
      softwareVersion: release?.version,
      datePublished: release?.date,
      screenshot: `${siteUrl}/og-image.jpg`,
      isAccessibleForFree: true,
      offers: free,
      publisher,
    };
  });

  const site = {
    "@context": "https://schema.org",
    "@type": "WebSite",
    name: "Disk Space",
    url: siteUrl,
    publisher,
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
        dangerouslySetInnerHTML={{__html: JSON.stringify(site)}}
      />
      {applications.map(application => (
        <script
          key={String(application.operatingSystem)}
          type="application/ld+json"
          dangerouslySetInnerHTML={{__html: JSON.stringify(application)}}
        />
      ))}
      <script
        type="application/ld+json"
        dangerouslySetInnerHTML={{__html: JSON.stringify(questions)}}
      />
    </>
  );
}
