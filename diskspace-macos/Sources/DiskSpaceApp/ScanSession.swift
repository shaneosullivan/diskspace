import AppKit
import DiskScanCore

/// Owns a scan and the derived state the UI reads.
///
/// The scan runs entirely off the main thread. The UI never blocks on it: it reads
/// the arena's published node count and whatever totals are current, which are
/// always self-consistent enough to draw. Nothing here takes a lock.
final class ScanSession {
    private(set) var scanner: TreeScanner?
    private(set) var arena: NodeArena?
    private(set) var rootPath: String = ""
    private(set) var isScanning = false
    private(set) var result: ScanResult?
    /// True once a scan was stopped part way, so the UI can say so rather than
    /// present a partial tree as a finished one.
    private(set) var wasStopped = false
    /// Sync-provider folders whose contents are not in these totals: left
    /// unopened because the scan was told to, or still being read when the scan
    /// stopped waiting. Either way what is inside them is missing.
    var unreadCloudFolders: Int { unreadCloudRoots.count }

    /// Which ones, by node, rather than how many.
    ///
    /// A refresh walks one folder and knows nothing about providers elsewhere in
    /// the tree, so it can neither replace this nor be added to it: taking the
    /// larger of two counts under-reported a refresh that found a second
    /// provider, and left the tree claiming one was missing after a refresh had
    /// just read it. Keeping the set, a refresh takes out what it covered and
    /// puts back what it found.
    private(set) var unreadCloudRoots: Set<Int32> = []
    private(set) var startedAt = Date()
    /// Nil when this root has never been scanned before.
    private(set) var sinceLastScanBaseline: Date?
    /// Which figure the UI is currently displaying.
    var metric: SizeMetric = .total
    /// Capacity of the drive holding the scanned folder, for the free-space and
    /// other-space blocks. Zero when it could not be determined.
    private(set) var volumeCapacity: Int64 = 0
    private(set) var volumeFree: Int64 = 0
    /// True when the scan root is the drive itself, in which case "other space"
    /// has no meaning.
    private(set) var scanningWholeVolume = false

    /// The folder a worker has been inside longest, and whether that has gone on
    /// long enough to be worth saying. Cloud-backed folders block for seconds at
    /// a time, and a frozen count with no explanation reads as a hang.
    private(set) var currentPath: String?
    private(set) var isStalled = false
    /// The scan is waiting on a sync provider's folder, and has been for long
    /// enough to be worth saying so. What the offer to skip keys off.
    private(set) var isStalledInCloudFolder = false
    /// True when the offer to skip should not be made: either it has been taken
    /// already, or the setting says to skip anyway so there is nothing to offer.
    private(set) var didOfferCloudSkip = false
    /// The user answered "keep waiting". Held here rather than in the window so
    /// that a refresh clears it along with everything else about the last walk;
    /// half of the decision resetting and half of it not left refreshes unable
    /// to offer at all.
    var cloudOfferDismissed = false

    /// Bumped whenever the UI should consider its cached derivations stale.
    private(set) var generation: Int = 0

    var onRefresh: (() -> Void)?

    private var timer: Timer?
    private var scanThread: Thread?

    func start(path: String, workers: Int? = nil) {
        cancel()
        rootPath = path
        startedAt = Date()
        readVolumeCapacity(for: path)
        var opts = ScanOptions()
        if let workers { opts.workers = workers }
        opts.skipCloudFolders = Preferences.shared.skipCloudFolders
        DebugLog.line("scan start: \(path) (workers \(opts.workers))")
        // Read the baseline before the scan records a new one.
        opts.applyScanHistory(for: path)
        currentPath = nil
        isStalled = false
        isStalledInCloudFolder = false
        didOfferCloudSkip = Preferences.shared.skipCloudFolders
        cloudOfferDismissed = false
        opts.progress = { [weak self] progress in
            // Off the scan thread, and read on the main one by the refresh timer.
            DispatchQueue.main.async {
                guard let self, self.isScanning else { return }
                self.currentPath = progress.currentPath
                self.isStalled = progress.stalled
                self.isStalledInCloudFolder = progress.stalledInCloudFolder
            }
        }
        sinceLastScanBaseline = opts.cutoffSinceLastScan.map {
            Date(timeIntervalSince1970: Double($0))
        }
        let scanner = TreeScanner(options: opts)
        self.scanner = scanner
        self.arena = scanner.arena
        isScanning = true
        result = nil
        wasStopped = false
        unreadCloudRoots = []
        generation &+= 1
        MenuBarStatus.show(.working)

        let thread = Thread { [weak self] in
            let r = scanner.scan(root: path)
            DispatchQueue.main.async {
                guard let self, self.scanner === scanner else { return }
                self.result = r
                self.isScanning = false
                self.wasStopped = r.stopped
                // Providers, not directories. Workers left behind are folded
                // into this by the scanner, so the two are not added up here.
                self.unreadCloudRoots = r.incompleteCloudRoots
                // Stopping ends the report as surely as finishing does: nothing
                // is being measured any more, so the badge goes rather than
                // pulsing away at a scan that is over.
                if r.stopped {
                    MenuBarStatus.clear()
                } else {
                    MenuBarStatus.show(.done)
                }
                DebugLog.line(
                    "scan complete: \(path) — \(r.files) files, \(r.directories) folders, "
                        + "\(r.arena.allocated[0]) bytes allocated, \(r.arena.logical[0]) logical, "
                        + "\(r.arena.count) nodes, \(r.errors) unreadable, "
                        + String(format: "%.1fs", r.elapsed))
                self.generation &+= 1
                self.stopTimer()
                self.onRefresh?()
            }
        }
        thread.stackSize = 1 << 20
        thread.start()
        scanThread = thread

        startRefreshTimer()
    }

    /// Rescans one folder in place, keeping the rest of the tree as it stands.
    ///
    /// Everything under the folder is discarded first — its bytes leave every
    /// total that included them — so a folder that has since been emptied ends
    /// up at zero rather than keeping figures nothing on disk supports.
    func refresh(node: Int32) {
        guard !isScanning, let scanner else { return }
        DebugLog.line("refresh node \(node)")
        startedAt = Date()
        isScanning = true
        result = nil
        wasStopped = false
        // `unreadCloudFolders` is deliberately not cleared: this walk covers one
        // folder and knows nothing about providers elsewhere in the tree, so
        // zeroing it would put a complete-looking total back on screen over a
        // tree that is still missing them.
        // The same clean slate a whole scan gets. Without it a refresh inherits
        // where the last scan was stuck, which is enough on its own to satisfy
        // the offer to skip: the banner appeared on the first tick of a refresh
        // of a purely local folder, naming a folder from the scan before it.
        currentPath = nil
        isStalled = false
        isStalledInCloudFolder = false
        // From the scanner that will run it, not from the preference: a
        // refresh goes back to the setting the scanner was built with, and the
        // preference may have changed since.
        didOfferCloudSkip = scanner.skipsCloudFoldersByDefault
        cloudOfferDismissed = false
        generation &+= 1
        MenuBarStatus.show(.working)

        let thread = Thread { [weak self] in
            let r = scanner.rescan(node: node)
            DispatchQueue.main.async {
                guard let self, self.scanner === scanner else { return }
                self.result = r
                self.isScanning = false
                self.wasStopped = r.stopped
                // What this folder was missing, in place of whatever it was
                // missing before, and leaving the rest of the tree's alone.
                self.unreadCloudRoots = self.unreadCloudRoots
                    .filter { !r.arena.isDescendant($0, ofOrEqualTo: node) }
                    .union(r.incompleteCloudRoots)
                // Stopping ends the report as surely as finishing does: nothing
                // is being measured any more, so the badge goes rather than
                // pulsing away at a scan that is over.
                if r.stopped {
                    MenuBarStatus.clear()
                } else {
                    MenuBarStatus.show(.done)
                }
                DebugLog.line(
                    "refresh complete: node \(node) — \(r.arena.count) nodes, "
                        + String(format: "%.1fs", r.elapsed))
                self.generation &+= 1
                self.stopTimer()
                self.onRefresh?()
            }
        }
        thread.stackSize = 1 << 20
        thread.start()
        scanThread = thread
        startRefreshTimer()
    }

    /// Takes an entry out of the totals, for something that has just been moved
    /// to the trash. The row stays, at zero.
    func remove(node: Int32) {
        guard !isScanning, let scanner else { return }
        scanner.remove(node: node)
        generation &+= 1
        onRefresh?()
    }

    /// Drives the file list and kind statistics, which are cheap to redraw. The
    /// treemap paces itself off this tick, since it costs far more.
    private func startRefreshTimer() {
        stopTimer()
        let hz = ProcessInfo.processInfo.environment["DISKSPACE_REFRESH_HZ"]
            .flatMap(Double.init) ?? 10.0
        let interval = 1.0 / max(0.2, min(hz, 60))
        DebugLog.line(String(format: "ui refresh interval: %.3fs (%.1f Hz)", interval, hz))
        let t = Timer(timeInterval: interval, repeats: true) { [weak self] _ in
            guard let self, self.isScanning else { return }
            self.generation &+= 1
            self.onRefresh?()
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private func readVolumeCapacity(for path: String) {
        volumeCapacity = 0
        volumeFree = 0
        scanningWholeVolume = false
        let url = URL(fileURLWithPath: path)
        let keys: Set<URLResourceKey> = [
            .volumeTotalCapacityKey, .volumeAvailableCapacityKey, .volumeURLKey,
        ]
        guard let values = try? url.resourceValues(forKeys: keys) else { return }
        volumeCapacity = Int64(values.volumeTotalCapacity ?? 0)
        volumeFree = Int64(values.volumeAvailableCapacity ?? 0)
        if let mount = values.volume {
            scanningWholeVolume =
                mount.standardizedFileURL.path == url.standardizedFileURL.path
        }
    }

    /// Leaves the rest of the sync providers' folders unread, for this scan only.
    ///
    /// The setting is not touched: this is an answer to "this one is taking
    /// forever", not a decision about every scan from now on.
    func skipCloudFoldersForThisScan() {
        guard isScanning, let scanner else { return }
        DebugLog.line("skipping cloud folders for the rest of this scan")
        scanner.setSkipCloudFolders(true)
        didOfferCloudSkip = true
        isStalledInCloudFolder = false
        onRefresh?()
    }

    /// Stops the walk but keeps everything found so far on screen.
    func stop() {
        guard isScanning else { return }
        scanner?.stop()
    }

    /// Stops the walk and throws away the results with it.
    func cancel() {
        // Without this the workers would keep reading the disk for a tree
        // nothing is going to display.
        scanner?.stop()
        stopTimer()
        // Nothing is being measured any more, so nothing should be reported.
        MenuBarStatus.clear()
        scanner = nil
        arena = nil
        isScanning = false
        wasStopped = false
        unreadCloudRoots = []
    }

    private func stopTimer() {
        timer?.invalidate()
        timer = nil
    }

    var elapsed: TimeInterval {
        result.map { $0.elapsed } ?? Date().timeIntervalSince(startedAt)
    }
}

enum Format {
    static func bytes(_ n: Int64) -> String {
        let units = ["B", "kB", "MB", "GB", "TB", "PB"]
        var v = Double(n)
        var u = 0
        while v >= 1000 && u < units.count - 1 {
            v /= 1000
            u += 1
        }
        return u == 0 ? "\(n) B" : String(format: "%.1f %@", v, units[u])
    }

    private static let counter: NumberFormatter = {
        let f = NumberFormatter()
        f.numberStyle = .decimal
        return f
    }()

    static func count(_ n: Int) -> String {
        counter.string(from: NSNumber(value: n)) ?? "\(n)"
    }
}
