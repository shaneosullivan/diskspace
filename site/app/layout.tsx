import type {Metadata} from "next";
import {Geist, Geist_Mono} from "next/font/google";
import {siteUrl} from "@/lib/site";
import "./globals.css";

const geistSans = Geist({variable: "--font-geist-sans", subsets: ["latin"]});
const geistMono = Geist_Mono({
  variable: "--font-geist-mono",
  subsets: ["latin"],
});

const title = "Disk Space - See Where Your Disk Space Went";
const description =
  "Free disk usage analyser for macOS that scans faster than light. Results appear instantly, quickly find new files that fill your computer.";

export const metadata: Metadata = {
  metadataBase: new URL(siteUrl),
  title,
  description,
  keywords: [
    "disk usage",
    "disk space analyser",
    "treemap",
    "macOS",
    "storage",
  ],
  icons: {
    icon: [{url: "/favicon-96x96.png", sizes: "96x96", type: "image/png"}],
    apple: "/apple-touch-icon.png",
  },
  openGraph: {
    title,
    description,
    type: "website",
    images: ["/screenshot.jpg"],
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
