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
  },
};

export default function RootLayout({
  children,
}: Readonly<{children: React.ReactNode}>) {
  return (
    <html lang="en">
      <body className={`${geistSans.variable} ${geistMono.variable}`}>
        {children}
      </body>
    </html>
  );
}
