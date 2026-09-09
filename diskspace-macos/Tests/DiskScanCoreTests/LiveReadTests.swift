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

        // Counts only ever rise during a walk, so a reader that sees one go
        // backwards has read a half-written figure. Checking that is worth more
        // than checking an array has a non-negative size, which was the whole of
        // what this asserted before.
        let seen = Counts()
        let reading = Thread {
            for _ in 0..<4000 {
                seen.record(scanner.kindTotals())
                usleep(200)
            }
        }
        reading.start()

        let result = scanner.scan(root: root.path)
        XCTAssertEqual(result.files, 600)

        // The reader first: it is still writing what is asserted below, and
        // reading that while it runs is a race of the test's own making.
        while !reading.isFinished { usleep(1000) }
        XCTAssertTrue(seen.everRose, "the reader should have seen the scan progressing")
        XCTAssertFalse(seen.everFell, "a count going backwards is a half-written figure")
    }

    /// `liveNodeCount` subtracts a figure that a refresh changes, and the window
    /// reads it throughout. The refresh is the writer here: an ordinary walk
    /// never touches it after the first line, so reading during one proved
    /// nothing.
    func testTheLiveNodeCountCanBeReadWhileAFolderIsRefreshed() throws {
        let root = try makeTree()
        defer { TemporaryTree.remove(root) }

        var opts = ScanOptions()
        opts.workers = 4
        let scanner = TreeScanner(options: opts)
        let result = scanner.scan(root: root.path)
        let folder = try XCTUnwrap(
            (0..<result.arena.count).first { result.arena.name($0) == "d7" })

        let reading = Thread {
            for _ in 0..<3000 {
                XCTAssertGreaterThan(scanner.liveNodeCount, 0)
                usleep(200)
            }
        }
        reading.start()
        for _ in 0..<8 { _ = scanner.rescan(node: Int32(folder)) }
        while !reading.isFinished { usleep(1000) }
    }

    /// Highest and lowest seen, so a reader can say whether the figures moved
    /// and whether they ever moved the wrong way.
    private final class Counts: @unchecked Sendable {
        private let lock = NSLock()
        private var highest: [Int16: Int] = [:]
        private var fell = false
        private var rose = false

        var everFell: Bool {
            lock.lock()
            defer { lock.unlock() }
            return fell
        }

        var everRose: Bool {
            lock.lock()
            defer { lock.unlock() }
            return rose
        }

        func record(_ totals: [(kind: Int16, bytes: Int64, count: Int)]) {
            lock.lock()
            defer { lock.unlock() }
            for entry in totals {
                if let was = highest[entry.kind] {
                    if entry.count < was { fell = true }
                    if entry.count > was { rose = true }
                }
                highest[entry.kind] = max(highest[entry.kind] ?? 0, entry.count)
            }
        }
    }
}
