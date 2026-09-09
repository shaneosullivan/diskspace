import XCTest

@testable import DiskScanCore

/// Reading a scan while it runs, which is what the window does ten times a
/// second.
///
/// The rest of the suite scans and then looks at the answer, so nothing in it
/// ever touches the scanner mid-walk — and the accessors the app calls from the
/// main thread were racing the applier without a single test noticing. Run these
/// under `swift test --sanitize=thread`: without it they only prove the numbers
/// come out sane, which is worth something but not the point.
final class LiveReadTests: XCTestCase {

    /// A tree big enough that the scan outlasts a few hundred reads.
    private func makeTree() throws -> URL {
        var entries: [String: String] = [:]
        for i in 0..<150 {
            for j in 0..<4 {
                entries["d\(i)/s\(j)/file.txt"] = String(repeating: "x", count: 16)
            }
        }
        return try TemporaryTree.make(entries)
    }

    func testTheKindTotalsCanBeReadWhileTheScanIsRunning() throws {
        let root = try makeTree()
        defer { TemporaryTree.remove(root) }

        var opts = ScanOptions()
        opts.workers = 4
        let scanner = TreeScanner(options: opts)

        let reading = Thread {
            // Until the scan is over, as the window's refresh timer does.
            for _ in 0..<4000 {
                let totals = scanner.kindTotals()
                XCTAssertGreaterThanOrEqual(totals.count, 0)
                usleep(200)
            }
        }
        reading.start()

        let result = scanner.scan(root: root.path)
        XCTAssertEqual(result.files, 600)

        // Let the reader finish so the sanitizer sees the whole overlap.
        while !reading.isFinished { usleep(1000) }
    }

    func testTheLiveNodeCountCanBeReadWhileTheScanIsRunning() throws {
        let root = try makeTree()
        defer { TemporaryTree.remove(root) }

        var opts = ScanOptions()
        opts.workers = 4
        let scanner = TreeScanner(options: opts)

        let reading = Thread {
            for _ in 0..<4000 {
                XCTAssertGreaterThanOrEqual(scanner.liveNodeCount, 0)
                usleep(200)
            }
        }
        reading.start()

        _ = scanner.scan(root: root.path)
        while !reading.isFinished { usleep(1000) }
    }
}
