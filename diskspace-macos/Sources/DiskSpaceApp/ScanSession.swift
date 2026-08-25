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

    /// Bumped whenever the UI should consider its cached derivations stale.
    private(set) var generation: Int = 0

    var onRefresh: (() -> Void)?

    private var timer: Timer?
    private var scanThread: Thread?

    func start(path: String, workers: Int = 4) {
        cancel()
        rootPath = path
        startedAt = Date()
        readVolumeCapacity(for: path)
        DebugLog.line("scan start: \(path) (workers \(workers))")
        var opts = ScanOptions()
        opts.workers = workers
        // Read the baseline before the scan records a new one.
        opts.applyScanHistory(for: path)
        sinceLastScanBaseline = opts.cutoffSinceLastScan.map {
            Date(timeIntervalSince1970: Double($0))
        }
        let scanner = TreeScanner(options: opts)
        self.scanner = scanner
        self.arena = scanner.arena
        isScanning = true
        result = nil
        generation &+= 1

        let thread = Thread { [weak self] in
            let r = scanner.scan(root: path)
            DispatchQueue.main.async {
                guard let self, self.scanner === scanner else { return }
                self.result = r
                self.isScanning = false
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

        // Drives the file list and kind statistics, which are cheap to redraw.
        // The treemap paces itself off this tick, since it costs far more.
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

    func cancel() {
        stopTimer()
        scanner = nil
        arena = nil
        isScanning = false
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
