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

    /// Bumped whenever the UI should consider its cached derivations stale.
    private(set) var generation: Int = 0

    var onRefresh: (() -> Void)?

    private var timer: Timer?
    private var scanThread: Thread?

    func start(path: String, workers: Int = 4) {
        cancel()
        rootPath = path
        startedAt = Date()
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
                self.generation &+= 1
                self.stopTimer()
                self.onRefresh?()
            }
        }
        thread.stackSize = 1 << 20
        thread.start()
        scanThread = thread

        // One refresh per second while scanning. Measured cost to the scan is
        // under 1%; the work here is bounded by what is on screen, not tree size.
        let t = Timer(timeInterval: 1.0, repeats: true) { [weak self] _ in
            guard let self, self.isScanning else { return }
            self.generation &+= 1
            self.onRefresh?()
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t
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
