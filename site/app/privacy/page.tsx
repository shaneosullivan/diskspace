import type {Metadata} from "next";

import Footer from "@/components/Footer";
import Header from "@/components/Header";
import {siteUrl} from "@/lib/site";
import styles from "./page.module.css";

export const metadata: Metadata = {
  title: "Privacy",
  description:
    "What Disk Space and this website do and do not collect. The app measures your disk locally and sends nothing about your files; the site uses cookie-free analytics.",
  alternates: {canonical: "/privacy"},
  openGraph: {
    title: "Disk Space privacy",
    description:
      "What Disk Space and this website do and do not collect. Nothing about your files ever leaves your machine.",
    url: `${siteUrl}/privacy`,
    type: "website",
  },
};

/// Written from what the code actually does. If the app or the site starts
/// doing something else, this page is part of that change.
const updated = "26 August 2026";

export default function Privacy() {
  return (
    <>
      <Header />
      <main className="container">
        <h1 className={styles.title}>Privacy</h1>
        <p className={styles.lead}>
          Disk Space reads your disk. That is the whole point of it, and it is
          exactly why what happens to the results matters. The short version:
          nothing about your files ever leaves your machine, and there is no
          account to create.
        </p>
        <p className={styles.updated}>Last updated {updated}</p>

        <section className={styles.section}>
          <h2>The app</h2>
          <h3>What stays on your Mac</h3>
          <p>
            Every measurement — file names, folder structure, sizes, dates, the
            treemap, the totals — is computed on your machine and stays there.
            None of it is uploaded, and none of it is written anywhere outside
            your own disk.
          </p>
          <p>
            The app stores two things locally: your settings, and the date and
            time of the last scan of each folder. The second is what makes the
            &ldquo;added since last scan&rdquo; figure possible. Both live in
            the app&rsquo;s own preferences on your Mac and are removed when you
            delete it.
          </p>

          <h3>The one request it makes</h3>
          <p>
            When the app starts, and when you open the About window, it asks
            this website whether a newer version has been released. That request
            carries two things: which platform the app is running on (
            <code>macos</code>) and its own version number (for example{" "}
            <code>1.0.4</code>). It carries nothing about your files, your
            folders or your disk.
          </p>
          <p>
            Like any web request, it reaches the server with your IP address
            attached. We record the platform, the version and the time of the
            check, and use those to know which builds are still in use. We do
            not store your IP address alongside them.
          </p>
          <p>
            If the request fails, or you are offline, the app carries on as
            normal — the check is not a condition of using it.
          </p>

          <h3>What it never does</h3>
          <p>
            There is no account, no sign-in, no crash reporting, no usage
            telemetry, and no advertising or tracking code of any kind in the
            app.
          </p>
        </section>

        <section className={styles.section}>
          <h2>This website</h2>
          <h3>Analytics</h3>
          <p>
            The site uses{" "}
            <a href="https://www.simpleanalytics.com/">Simple Analytics</a>,
            which is built to measure traffic without identifying people. It
            sets no cookies, collects no personal data, and builds no profile of
            you across sites or visits. We see totals — pages viewed, where
            visitors arrived from, which download buttons were pressed and which
            platform and version they were for — never individuals.
          </p>
          <p>
            You can opt out of analytics entirely by turning on{" "}
            <em>Do Not Track</em> in your browser, which Simple Analytics
            honours.
          </p>

          <h3>The video</h3>
          <p>
            The demonstration video is embedded from YouTube using their
            no-cookie domain, so YouTube sets no cookies and receives no
            identifying request until you actually play it. Once you press play,
            YouTube&rsquo;s own{" "}
            <a href="https://policies.google.com/privacy">privacy policy</a>{" "}
            applies to what happens next.
          </p>

          <h3>Downloads and hosting</h3>
          <p>
            Application downloads are served from Google Firebase Storage, so
            pressing a download button makes a request to Google&rsquo;s
            servers. The site itself is hosted on Vercel, whose servers keep
            standard request logs, including IP addresses, for a limited period.
          </p>
          <p>
            Fonts are served from this site rather than fetched from a font
            provider, so no third party learns you were here from those.
          </p>
        </section>

        <section className={styles.section}>
          <h2>Children</h2>
          <p>
            Neither the app nor the site collects personal information from
            anyone, including children.
          </p>
        </section>

        <section className={styles.section}>
          <h2>Changes</h2>
          <p>
            If what the app or the site collects ever changes, this page changes
            with it, and the date at the top moves. The app&rsquo;s source is{" "}
            <a href="https://github.com/shaneosullivan/diskspace">public</a>, so
            what it does with the network is there to be read rather than taken
            on trust.
          </p>
        </section>

        <section className={styles.section}>
          <h2>Contact</h2>
          <p>
            Disk Space is made by{" "}
            <a href="https://www.chofter.com">Chofter Apps</a>. Questions about
            this policy, or about anything on it, go to{" "}
            <a href="mailto:support@diskspace.io">support@diskspace.io</a>.
          </p>
        </section>
      </main>
      <Footer />
    </>
  );
}
