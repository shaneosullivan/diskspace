import Darwin
import DiskScanCore
import Foundation

func humanBytes(_ n: Int64) -> String {
    let units = ["B", "kB", "MB", "GB", "TB", "PB"]
    var v = Double(n)
    var u = 0
    while v >= 1000 && u < units.count - 1 {
        v /= 1000
        u += 1
    }
    return u == 0 ? "\(n) B" : String(format: "%.1f %@", v, units[u])
}

func grouped(_ n: Int) -> String {
    let f = NumberFormatter()
    f.numberStyle = .decimal
    return f.string(from: NSNumber(value: n)) ?? "\(n)"
}

func printKindStatistics(_ arena: NodeArena, limit: Int) {
    var bytesByKind = [Int64](repeating: 0, count: FileKind.names.count)
    var countByKind = [Int](repeating: 0, count: FileKind.names.count)
    for i in 0..<arena.count where !arena.isDirectory(i) {
        let k = Int(arena.kind[i])
        bytesByKind[k] += arena.allocated[i]
        countByKind[k] += 1
    }
    let ranked = (0..<FileKind.names.count)
        .filter { countByKind[$0] > 0 }
        .sorted { bytesByKind[$0] > bytesByKind[$1] }
        .prefix(limit)

    print("\nTop file kinds by size")
    print(String(repeating: "-", count: 46))
    for k in ranked {
        print(
            String(
                format: "%-24@ %12@ %10@", FileKind.names[k] as NSString,
                humanBytes(bytesByKind[k]) as NSString, grouped(countByKind[k]) as NSString))
    }
}

func printLargest(_ arena: NodeArena, limit: Int, metric: SizeMetric = .total) {
    let ranked = (1..<arena.count)
        .filter { arena.size(metric, $0) > 0 }
        .sorted { arena.size(metric, $0) > arena.size(metric, $1) }
        .prefix(limit)
    print("\n\(metric.title): largest entries anywhere in the tree")
    print(String(repeating: "-", count: 74))
    for i in ranked {
        let marker = arena.isDirectory(i) ? "/" : " "
        print(
            String(
                format: "%12@ %@ %@", humanBytes(arena.size(metric, i)) as NSString,
                marker as NSString, arena.path(i) as NSString))
    }
}

/// Sanity check on the contiguous-children invariant the applier maintains.
func verifyChildRanges(_ arena: NodeArena) -> String {
    var checked = 0
    var bad = 0
    for i in 0..<arena.count where arena.isDirectory(i) {
        let range = arena.children(of: i)
        for c in range where Int(arena.parent[c]) != i { bad += 1 }
        checked += range.count
    }
    return bad == 0
        ? "child ranges OK (\(grouped(checked)) links verified)"
        : "CHILD RANGE MISMATCH: \(grouped(bad)) of \(grouped(checked))"
}

func runScan(path: String, workers: Int, backend: ScanBackend, quiet: Bool, verify: Bool) {
    var opts = ScanOptions()
    opts.workers = workers
    opts.backend = backend
    opts.applyScanHistory(for: path)
    if !quiet {
        opts.progress = { p in
            FileHandle.standardError.write(
                Data(
                    "\r  scanning… \(grouped(p.files)) files, \(grouped(p.directories)) dirs, \(humanBytes(p.allocatedBytes))    "
                        .utf8))
        }
    }

    let scanner = TreeScanner(options: opts)
    let r = scanner.scan(root: path)
    if !quiet { FileHandle.standardError.write(Data("\r\u{1B}[K".utf8)) }
    let a = r.arena

    print("Scanned \(path)")
    print(String(repeating: "=", count: 46))
    print("  backend            \(backend.rawValue)  (\(workers) worker\(workers == 1 ? "" : "s") + 1 applier)")
    print("  elapsed            \(String(format: "%.2f s", r.elapsed))")
    print("  files              \(grouped(r.files))")
    print("  directories        \(grouped(r.directories))")
    print("  throughput         \(grouped(Int(Double(a.count) / r.elapsed))) entries/sec")
    print("  logical size       \(humanBytes(a.logical[0]))")
    print("  allocated size     \(humanBytes(a.allocated[0]))")
    print("  exact bytes        \(a.allocated[0]) alloc / \(a.logical[0]) logical")
    print("  exact entries      \(a.count) nodes / \(r.files) files")
    let stamped = Double(r.entriesWithTimestamp) / Double(max(a.count - 1, 1)) * 100
    print("  --- recency ---")
    print("  added last month   \(humanBytes(a.addedLastMonth[0]))")
    print("  added last 6 mths  \(humanBytes(a.addedLastSixMonths[0]))")
    if let baseline = r.sinceLastScanBaseline {
        let f = DateFormatter()
        f.dateStyle = .medium
        f.timeStyle = .short
        print("  added since scan   \(humanBytes(a.addedSinceLastScan[0]))  (baseline \(f.string(from: baseline)))")
    } else {
        print("  added since scan   unavailable — first scan of this root")
    }
    print(String(format: "  timestamp coverage %.1f%% of entries", stamped))
    print("  hardlink dupes     \(grouped(r.hardlinkDuplicates)) (\(humanBytes(r.hardlinkDuplicateBytes)) not double-counted)")
    print("  unreadable         \(grouped(r.errors))")
    print("  applier queue      max depth \(r.queueMaxDepth), \(grouped(r.queueBlockedPushes)) blocked pushes")
    print("  node memory        \(humanBytes(Int64(a.approximateMemoryBytes())))")
    print("  --- where the time goes (summed across threads) ---")
    print(String(format: "  worker total       %.2fs   buildPath %.2fs | open %.2fs | read %.2fs | push %.2fs",
                 r.secWorkerTotal, r.secBuildPath, r.secOpen, r.secRead, r.secPush))
    print(String(format: "  applier            %.2fs busy of %.2fs alive (%.1f%% of wall clock)",
                 r.secApplier, r.secApplierTotal, r.secApplier / max(r.elapsed, 1e-9) * 100))
    if verify { print("  invariant          \(verifyChildRanges(a))") }

    if !quiet {
        printKindStatistics(a, limit: 20)
        printLargest(a, limit: 12)
        printLargest(a, limit: 12, metric: .addedLastMonth)
    }
}

func runBench(path: String, repeats: Int, maxWorkers: Int) {
    struct Row { var label: String; var best: Double; var entries: Int; var depth: Int }
    var rows: [Row] = []

    var configs: [(String, ScanBackend, Int)] = [
        ("readdir+fstatat, 1 worker", .stat, 1),
        ("getattrlistbulk, 1 worker", .bulk, 1),
    ]
    var w = 2
    while w <= maxWorkers {
        configs.append(("readdir+fstatat, \(w) workers", .stat, w))
        configs.append(("getattrlistbulk, \(w) workers", .bulk, w))
        w *= 2
    }

    for (label, backend, workers) in configs {
        var best = Double.infinity
        var entries = 0
        var depth = 0
        for _ in 0..<repeats {
            var opts = ScanOptions()
            opts.backend = backend
            opts.workers = workers
            let r = TreeScanner(options: opts).scan(root: path)
            if r.elapsed < best {
                best = r.elapsed
                entries = r.arena.count
                depth = r.queueMaxDepth
            }
        }
        rows.append(Row(label: label, best: best, entries: entries, depth: depth))
        FileHandle.standardError.write(
            Data("  \(label): \(String(format: "%.2fs", best))\n".utf8))
    }

    let baseline = rows.first?.best ?? 1
    print("\n\(grouped(rows.first?.entries ?? 0)) entries — \(path)")
    print(String(repeating: "-", count: 76))
    print(
        String(
            format: "%-30@ %10@ %14@ %9@ %8@", "configuration" as NSString, "best" as NSString,
            "entries/sec" as NSString, "speedup" as NSString, "q depth" as NSString))
    print(String(repeating: "-", count: 76))
    for r in rows {
        print(
            String(
                format: "%-30@ %9.2fs %14@ %8.1fx %8d", r.label as NSString, r.best,
                grouped(Int(Double(r.entries) / r.best)) as NSString, baseline / r.best, r.depth))
    }
    print(String(repeating: "-", count: 76))
}

/// Prices the two things the profile says dominate: how many bytes we hand
/// getattrlistbulk, which attributes we ask for, and full-path open() versus
/// openat() against a parent descriptor we already hold.
func runIOBench(path: String, workers: Int) {
    print("I/O experiments — \(path)\n")

    print("getattrlistbulk buffer size")
    print(String(repeating: "-", count: 52))
    for kb in [32, 64, 256, 1024, 4096] {
        var opts = ScanOptions()
        opts.workers = workers
        opts.bulkBufferSize = kb * 1024
        let r = TreeScanner(options: opts).scan(root: path)
        print(String(format: "  %5d KB   %6.2fs   read %6.2fs   %@ entries/sec",
                     kb, r.elapsed, r.secRead,
                     grouped(Int(Double(r.arena.count) / r.elapsed)) as NSString))
    }

    print("\nattribute set")
    print(String(repeating: "-", count: 52))
    for profile in AttributeProfile.allCases {
        var opts = ScanOptions()
        opts.workers = workers
        opts.attributeProfile = profile
        let r = TreeScanner(options: opts).scan(root: path)
        print(String(format: "  %-10@ %6.2fs   read %6.2fs   %@ entries/sec",
                     profile.rawValue as NSString, r.elapsed, r.secRead,
                     grouped(Int(Double(r.arena.count) / r.elapsed)) as NSString))
    }

    // Collect real directories, with their parent, to compare the two open forms.
    var opts = ScanOptions()
    opts.workers = workers
    let scan = TreeScanner(options: opts).scan(root: path)
    let arena = scan.arena
    var samples: [(parent: String, name: String, full: String)] = []
    var depths = 0
    var i = 1
    while i < arena.count && samples.count < 20000 {
        if arena.isDirectory(i), arena.parent[i] != 0 {
            let full = arena.path(i)
            let parent = arena.path(Int(arena.parent[i]))
            samples.append((parent, arena.name(i), full))
            depths += full.reduce(0) { $1 == "/" ? $0 + 1 : $0 }
        }
        i += max(1, arena.count / 40000)
    }
    guard !samples.isEmpty else { print("\nno directories sampled"); return }

    print("\nopen() vs openat()  —  \(grouped(samples.count)) directories, mean depth \(depths / samples.count)")
    print(String(repeating: "-", count: 62))

    // Open every parent up front, as a depth-first walker would already hold them.
    var parentFds: [String: Int32] = [:]
    for s in samples where parentFds[s.parent] == nil {
        parentFds[s.parent] = s.parent.withCString { open($0, O_RDONLY | O_DIRECTORY | O_CLOEXEC) }
    }
    defer { for (_, fd) in parentFds where fd >= 0 { close(fd) } }

    func timeFullPath() -> Double {
        let t0 = DispatchTime.now().uptimeNanoseconds
        var n = 0
        for s in samples {
            let fd = s.full.withCString { open($0, O_RDONLY | O_DIRECTORY | O_CLOEXEC) }
            if fd >= 0 { n += 1; close(fd) }
        }
        let ns = DispatchTime.now().uptimeNanoseconds - t0
        return Double(ns) / 1000 / Double(max(n, 1))
    }
    func timeOpenat() -> Double {
        let t0 = DispatchTime.now().uptimeNanoseconds
        var n = 0
        for s in samples {
            guard let pfd = parentFds[s.parent], pfd >= 0 else { continue }
            let fd = s.name.withCString { openat(pfd, $0, O_RDONLY | O_DIRECTORY | O_CLOEXEC) }
            if fd >= 0 { n += 1; close(fd) }
        }
        let ns = DispatchTime.now().uptimeNanoseconds - t0
        return Double(ns) / 1000 / Double(max(n, 1))
    }

    // The first pass over these paths warms the kernel's name cache, so an
    // unwarmed first measurement flatters whichever form runs second. Warm both,
    // then alternate, and report each pass so any remaining drift is visible.
    _ = timeFullPath()
    _ = timeOpenat()

    var fullRuns: [Double] = []
    var atRuns: [Double] = []
    for round in 0..<3 {
        if round % 2 == 0 {
            fullRuns.append(timeFullPath())
            atRuns.append(timeOpenat())
        } else {
            atRuns.append(timeOpenat())
            fullRuns.append(timeFullPath())
        }
    }
    let fullBest = fullRuns.min() ?? 0
    let atBest = atRuns.min() ?? 0
    print("  open(full path)    passes: "
        + fullRuns.map { String(format: "%.2f", $0) }.joined(separator: ", ") + " us/dir")
    print("  openat(parent fd)  passes: "
        + atRuns.map { String(format: "%.2f", $0) }.joined(separator: ", ") + " us/dir")
    print(String(format: "  best: full %.2f us  vs  openat %.2f us   ratio %.2fx",
                 fullBest, atBest, fullBest / max(atBest, 1e-9)))
    print(String(format: "  projected saving on a %@-directory scan: %.1fs",
                 grouped(samples.count) as NSString,
                 (fullBest - atBest) * Double(samples.count) / 1e6))
}

func usage() -> Never {
    print(
        """
        diskscan — parallel APFS tree scanner

        USAGE
          diskscan scan <path> [--workers N] [--backend bulk|stat] [--quiet] [--verify]
          diskscan bench <path> [--repeat N] [--max-workers N]
          diskscan iobench <path> [--workers N]

        OPTIONS
          --workers N       I/O worker threads (default 4); one applier thread always
          --backend         bulk = getattrlistbulk, stat = readdir + fstatat
          --verify          check the contiguous-children invariant after scanning
          --repeat N        benchmark passes per configuration (default 3, best wins)
          --max-workers N   highest worker count to benchmark (default 8)
        """)
    exit(1)
}

var args = Array(CommandLine.arguments.dropFirst())
guard let command = args.first else { usage() }
args.removeFirst()

func flagValue(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return args[i + 1]
}
func hasFlag(_ name: String) -> Bool { args.contains(name) }

var rawPositionals: [String] = []
var skipNext = false
for a in args {
    if skipNext { skipNext = false; continue }
    if a.hasPrefix("--") {
        if ["--workers", "--backend", "--repeat", "--max-workers"].contains(a) { skipNext = true }
        continue
    }
    rawPositionals.append(a)
}

guard let target = rawPositionals.first else { usage() }
let resolved = (target as NSString).expandingTildeInPath

switch command {
case "scan":
    runScan(
        path: resolved,
        workers: Int(flagValue("--workers") ?? "") ?? ScanOptions().workers,
        backend: ScanBackend(rawValue: flagValue("--backend") ?? "bulk") ?? .bulk,
        quiet: hasFlag("--quiet"), verify: hasFlag("--verify"))
case "iobench":
    runIOBench(path: resolved, workers: Int(flagValue("--workers") ?? "") ?? 4)
case "bench":
    runBench(
        path: resolved, repeats: Int(flagValue("--repeat") ?? "") ?? 3,
        maxWorkers: Int(flagValue("--max-workers") ?? "") ?? 8)
default:
    usage()
}
