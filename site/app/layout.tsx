import type {Metadata} from "next";
import {Geist, Geist_Mono} from "next/font/google";
import {siteUrl} from "@/lib/site";
import "./globals.css";

const geistSans = Geist({variable: "--font-geist-sans", subsets: ["latin"]});
const geistMono = Geist_Mono({
  variable: "--font-geist-mono",
  subsets: ["latin"],
});

const title = "Disk Space — Free Disk Usage Analyzer for Mac, Windows, Linux";
const description =
  "Find what is filling your Mac, PC or Linux box. A free disk usage analyser that scans a folder or whole drive in seconds, maps every file by size, and shows what arrived recently as well as what is big.";

export const metadata: Metadata = {
  metadataBase: new URL(siteUrl),
  title: {
    default: title,
    // Every other page reads "<its name> — Disk Space".
    template: "%s — Disk Space",
  },
  description,
  applicationName: "Disk Space",
  authors: [{name: "Chofter Apps", url: "https://www.chofter.com"}],
  creator: "Chofter Apps",
  publisher: "Chofter Apps",
  category: "technology",
  keywords: [
    "disk usage analyzer",
    "disk usage analyser",
    "disk space analyzer mac",
    "mac storage",
    "free up disk space mac",
    "what is taking up space on my mac",
    "disk space analyzer windows",
    "what is taking up space on my pc",
    "disk usage analyzer linux",
    "windirstat alternative",
    "treemap disk usage",
    "treemap",
    "macOS",
    "Windows",
    "Linux",
  ],
  alternates: {canonical: "/"},
  icons: {
    icon: [{url: "/favicon-96x96.png", sizes: "96x96", type: "image/png"}],
    apple: "/apple-touch-icon.png",
  },
  robots: {
    index: true,
    follow: true,
    googleBot: {
      index: true,
      follow: true,
      // Lets Google show the screenshot at full size rather than a thumbnail.
      "max-image-preview": "large",
      "max-snippet": -1,
    },
  },
  openGraph: {
    title,
    description,
    type: "website",
    url: siteUrl,
    siteName: "Disk Space",
    locale: "en_US",
    images: [
      {
        url: "/og-image.jpg",
        width: 1200,
        height: 630,
        alt: "The Disk Space window: file kinds and folders ranked by size, a treemap of a scanned drive, and files waiting to be deleted",
      },
    ],
  },
  twitter: {
    card: "summary_large_image",
    title,
    description,
    images: ["/og-image.jpg"],
  },
};

export default function RootLayout({
  children,
}: Readonly<{children: React.ReactNode}>) {
  return (
    <html lang="en">
      <body className={`${geistSans.variable} ${geistMono.variable}`}>
        {children}
        {/*
          Their documented queue stub: the script below is async, so a click
          landing before it arrives would otherwise be dropped. Calls made in
          the meantime are held and replayed once it loads.
        */}
        <script
          dangerouslySetInnerHTML={{
            __html:
              "window.sa_event=window.sa_event||function(){" +
              "var a=[].slice.call(arguments);" +
              "window.sa_event.q?window.sa_event.q.push(a):window.sa_event.q=[a]};",
          }}
        />
        <script
          async
          src="https://scripts.simpleanalyticscdn.com/latest.js"></script>
      </body>
    </html>
  );
}
