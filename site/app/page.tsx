import DemoVideo from "@/components/DemoVideo";
import StructuredData from "@/components/StructuredData";
import Header from "@/components/Header";
import Footer from "@/components/Footer";
import DownloadSection from "@/components/DownloadSection";
import {faq} from "@/lib/faq";
import {
  platformNames,
  platformRequirements,
  releasedPlatforms,
  unreleasedPlatforms,
} from "@/lib/releases";
import styles from "./page.module.css";

const measures = [
  {
    name: "Current size",
    body: "The classic view. Everything ranked by what it occupies right now.",
  },
  {
    name: "Added in the last month",
    body: "Only what was written in the last 30 days. The answer is usually here.",
    highlight: true,
  },
  {
    name: "Added in the last six months",
    body: "The slow creep you never notice until the day the disk is full.",
  },
  {
    name: "Added since last scan",
    body: "Scan, run the build or the sync, scan again, and see exactly what it left behind.",
  },
];

const features = [
  {
    title: "Results while it scans",
    body: "A partial tree and map are on screen within a second of starting, and sharpen as the walk proceeds. Nothing waits for the end.",
  },
  {
    title: "A map of the whole disk",
    body: "Every file drawn in proportion to its size and coloured by kind. Selecting a folder outlines it in place, so drilling down walks the highlight into the same picture rather than replacing it.",
  },
  {
    title: "Nothing leaves your machine",
    body: "No account, no tracking. What is on your disk stays a matter between you and your disk.",
  },
  {
    title: "Honest numbers",
    body: "Hard links are counted once, and logical size is tracked separately from space actually occupied. On a developer machine those two figures differ enormously.",
  },
  {
    title: "Sorted by what matters",
    body: "Folders ranked by size with the size column pinned, so it stays visible however deep the names get. Every file kind aggregated and sortable.",
  },
  {
    title: "Scriptable",
    body: "The same engine ships as a command line tool, for scanning, verifying totals and benchmarking your own hardware.",
  },
];

// Built from the releases rather than written out, so this cannot go on saying
// macOS only after Windows ships - which is exactly what it did.
const requirementsNote = `Free. ${releasedPlatforms()
  .map(platform => platformRequirements[platform])
  .join(" · ")}`;

// Likewise for what is still to come: naming a platform that has since shipped
// is the same mistake in the other direction. Empty once every platform is out,
// and the line goes with it.
const pending = unreleasedPlatforms().map(platform => platformNames[platform]);
const pendingNote =
  pending.length === 0
    ? null
    : pending.length === 1
      ? `A ${pending[0]} version is on the way.`
      : `${pending.join(" and ")} versions are on the way.`;

export default function Home() {
  return (
    <>
      <Header />
      <main>
        <section className={styles.hero}>
          <div className="container">
            <h1 className={styles.heroTitle}>
              Find your missing disk space,{" "}
              <span className="gradient-text">faster than light</span>
            </h1>
            <p className={styles.heroSubtitle}>
              A free disk usage analyser for macOS and Windows that shows where
              the space went while it is still looking — including what only
              turned up last week, which is usually the thing you are hunting
              for.
            </p>
            <div className={styles.heroCta}>
              <DownloadSection />
            </div>
            <p className={styles.heroNote}>{requirementsNote}</p>
          </div>
        </section>

        <section className={styles.videoSection}>
          <div className="container">
            <div className={styles.videoFrame}>
              <DemoVideo />
            </div>
          </div>
        </section>

        <section id="recent" className={styles.recent}>
          <div className="container">
            <h2 className={styles.sectionTitle}>
              The disk was fine last month
            </h2>
            <p className={styles.sectionSubtitle}>
              Then something arrived and filled it. Every folder carries four
              numbers here, not one, so the whole disk can be ranked by what is
              new rather than by what is merely big — and whatever turned up
              last week rises straight to the top.
            </p>
            <div className={styles.measureGrid}>
              {measures.map(measure => (
                <div
                  key={measure.name}
                  className={`${styles.measure} ${
                    measure.highlight ? styles.measureHighlight : ""
                  }`}>
                  <span className={styles.measureName}>{measure.name}</span>
                  <p className={styles.measureBody}>{measure.body}</p>
                </div>
              ))}
            </div>
            <p className={styles.footnote}>
              Both the list and the map follow whichever measure you pick, so a
              folder that looks harmless by total size can become the biggest
              thing on screen the moment you ask what is new.
            </p>
          </div>
        </section>

        <section id="speed" className={styles.speed}>
          <div className="container">
            <h2 className={styles.sectionTitle}>About that speed</h2>
            <div className={styles.stat}>
              <span className={`${styles.statNumber} gradient-text`}>
                150,000+
              </span>
              <span className={styles.statLabel}>
                files read per second
                <span className={styles.asterisk} aria-hidden="true">
                  *
                </span>
              </span>
            </div>
            <p className={styles.sectionSubtitle}>
              A home folder of 3.8 million files, measured end to end in under
              half a minute — and the first results are on screen a second after
              you press go.
            </p>
            <p className={styles.smallprint}>
              * Measured on an M1 MacBook Pro Max with a 1 TB drive.
            </p>
          </div>
        </section>

        <section id="features" className={styles.features}>
          <div className="container">
            <h2 className={styles.sectionTitle}>What it does</h2>
            <div className={styles.featureGrid}>
              {features.map(feature => (
                <div key={feature.title} className={styles.featureCard}>
                  <h3>{feature.title}</h3>
                  <p>{feature.body}</p>
                </div>
              ))}
            </div>
          </div>
        </section>

        <section id="download" className={styles.download}>
          <div className="container">
            <h2 className={styles.sectionTitle}>Download</h2>
            {pendingNote ? (
              <p className={styles.sectionSubtitle}>{pendingNote}</p>
            ) : null}
            <DownloadSection />
          </div>
        </section>
        <section id="faq" className={styles.faq}>
          <div className="container">
            <h2 className={styles.sectionTitle}>Questions</h2>
            <div className={styles.faqList}>
              {faq.map(entry => (
                <details key={entry.question} className={styles.faqItem}>
                  <summary className={styles.faqQuestion}>
                    {entry.question}
                  </summary>
                  <p className={styles.faqAnswer}>{entry.answer}</p>
                </details>
              ))}
            </div>
          </div>
        </section>
      </main>
      <Footer />
      <StructuredData />
    </>
  );
}
