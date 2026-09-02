/// The questions people actually ask before installing a disk usage tool.
///
/// One list feeds both the section on the page and the FAQPage structured data,
/// so the answers a search engine is shown are the answers a reader is shown.
export const faq: Array<{question: string; answer: string}> = [
  {
    question: "Is Disk Space free?",
    answer:
      "Yes. It is free to download and use, with no account, no subscription and no trial period.",
  },
  {
    question: "Which versions of macOS does it run on?",
    answer:
      "macOS 15 or later, on both Apple silicon and Intel Macs. There are Windows and Linux versions as well.",
  },
  {
    question: "Is there a Windows version?",
    answer:
      "Yes, for Windows 10 or later. It is called Disk Space ++ there, because the plain name was already taken on the Microsoft Store — it is otherwise the same application. The download is a single file with nothing to install and no runtime to install either, and it counts hard links, sparse files and compressed files the way the disk itself counts them.",
  },
  {
    question: "Why does it ask for Full Disk Access?",
    answer:
      "macOS hides folders like Mail, Messages and Photos from apps that have not been granted Full Disk Access. Without it the scan still runs, but those folders are skipped and the totals come out lower than the real figure. The app tells you when the permission is missing.",
  },
  {
    question: "Does it send anything about my files anywhere?",
    answer:
      "No. Every measurement stays on your computer. The only network request the app makes is asking the website whether a newer version has been released, which sends nothing but the version and platform it is running.",
  },
  {
    question: "How long does a scan take?",
    answer:
      "A home folder of 3.8 million files takes under half a minute on an M1 MacBook Pro Max. Results appear while the scan runs rather than at the end, so the largest folders are usually visible within a second of starting.",
  },
  {
    question: "How is it different from using du in the terminal?",
    answer:
      "It reads the same figures — totals are verified byte for byte against du and find — but draws every file in proportion to its size, counts hard links once, and lets you rank folders by what arrived recently rather than only by what is big. A command line tool ships alongside it if you prefer the terminal.",
  },
  {
    question: "Can it delete files?",
    answer:
      "It can move a selected file or folder to the Trash, after asking. Nothing is deleted outright, and nothing is removed without you choosing it.",
  },
  {
    question: "What does “added in the last month” mean?",
    answer:
      "Every folder carries four figures: its current size, and how many bytes arrived in the last month, the last six months, and since you last scanned that folder. Ranking by one of those shows what has been growing, which is usually what filled the disk.",
  },
];
