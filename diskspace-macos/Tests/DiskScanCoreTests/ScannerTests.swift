import XCTest

@testable import DiskScanCore

/// End-to-end walks over throwaway trees.
///
/// These are the tests that would catch the thing worth being afraid of here:
/// that recognising cloud folders, or giving up on a stuck worker, changes what
/// an ordinary scan reports.
final class ScannerTests: XCTestCase {

    func testScanCountsAnOrdinaryTree() throws {
        let root = try TemporaryTree.make([
            "one.txt": "12345",
            "sub/two.txt": "1234567890",
            "sub/deeper/three.txt": "x",
            "empty/": "",
        ])
        defer { TemporaryTree.remove(root) }

        let result = TreeScanner(options: quietOptions()).scan(root: root.path)
        XCTAssertEqual(result.files, 3)
        // root, sub, sub/deeper, empty
        XCTAssertEqual(result.directories, 4)
        XCTAssertEqual(result.errors, 0)
        XCTAssertFalse(result.stopped)
        XCTAssertEqual(result.abandonedWorkers, 0)
        XCTAssertGreaterThan(result.arena.allocated[0], 0)
        // Directory nodes carry a length of their own, so the root's total is
        // not simply the bytes in the files. Add up the files themselves.
        var fileBytes: Int64 = 0
        for i in 0..<result.arena.count where result.arena.flags[i] & NodeFlags.isDir == 0 {
            fileBytes += result.arena.logical[i]
        }
        XCTAssertEqual(fileBytes, 16, "5 + 10 + 1 bytes of content")
    }

    /// Skipping is about provider folders only. With no provider in the tree,
    /// turning it on must change nothing at all.
    func testSkippingChangesNothingWhenThereIsNoProvider() throws {
        let root = try TemporaryTree.make([
            "a/one.txt": "hello",
            "b/c/two.txt": "world!",
            "b/d/": "",
        ])
        defer { TemporaryTree.remove(root) }

        var skipping = quietOptions()
        skipping.skipCloudFolders = true

        let plain = TreeScanner(options: quietOptions()).scan(root: root.path)
        let skipped = TreeScanner(options: skipping).scan(root: root.path)

        XCTAssertEqual(plain.files, skipped.files)
        XCTAssertEqual(plain.directories, skipped.directories)
        XCTAssertEqual(plain.arena.allocated[0], skipped.arena.allocated[0])
        XCTAssertEqual(plain.arena.logical[0], skipped.arena.logical[0])
        XCTAssertEqual(skipped.abandonedWorkers, 0)
    }

    /// Turning skipping on part way through must not strand a walk that has no
    /// provider folders in it: every worker is busy with local directories, none
    /// may be abandoned, and the answer must come out whole.
    ///
    /// Driven from the progress callback, which is the one hook that runs during
    /// the walk. The reporter takes its first reading before the workers are
    /// started, so this lands mid-scan rather than after it — which the previous
    /// version of this test did not, making it an assertion about nothing.
    func testTurningSkippingOnMidScanStillCompletesNormally() throws {
        var entries: [String: String] = [:]
        for i in 0..<200 { entries["dir\(i)/file.txt"] = String(repeating: "x", count: 8) }
        let root = try TemporaryTree.make(entries)
        defer { TemporaryTree.remove(root) }

        let hook = ProgressHook()
        var opts = quietOptions()
        opts.progress = { _ in hook.fire() }
        let scanner = TreeScanner(options: opts)
        hook.action = { scanner.setSkipCloudFolders(true) }

        let result = scanner.scan(root: root.path)

        XCTAssertTrue(hook.fired, "the hook has to have run for this to mean anything")
        XCTAssertEqual(result.files, 200, "a local tree is counted in full regardless")
        XCTAssertEqual(result.directories, 201)
        XCTAssertEqual(
            result.abandonedWorkers, 0,
            "no worker was in a provider folder, so none may be left behind")
    }

    /// Stopping is reported, and stopping is also how abandoning releases idle
    /// workers, so it is worth knowing it still works.
    func testStoppingPartWayIsReportedAsPartial() throws {
        var entries: [String: String] = [:]
        for i in 0..<400 { entries["d\(i)/f.txt"] = "x" }
        let root = try TemporaryTree.make(entries)
        defer { TemporaryTree.remove(root) }

        let hook = ProgressHook()
        var opts = quietOptions()
        opts.progress = { _ in hook.fire() }
        let scanner = TreeScanner(options: opts)
        hook.action = { scanner.stop() }

        let result = scanner.scan(root: root.path)
        XCTAssertTrue(hook.fired, "the hook has to have run for this to mean anything")
        XCTAssertTrue(result.stopped, "a walk cut short should say so")
    }

    /// Lets a test act from inside the scan without the scanner and the closure
    /// each needing to exist before the other, and without the progress thread
    /// and the test racing over a plain `var`.
    final class ProgressHook: @unchecked Sendable {
        private let lock = NSLock()
        private var didFire = false
        private var stored: (() -> Void)?

        var action: (() -> Void)? {
            get {
                lock.lock()
                defer { lock.unlock() }
                return stored
            }
            set {
                lock.lock()
                stored = newValue
                lock.unlock()
            }
        }

        /// Runs the action once. Once is enough for both callers, and it keeps a
        /// ten-times-a-second callback from repeating the work.
        func fire() {
            lock.lock()
            let first = !didFire
            didFire = true
            let action = stored
            lock.unlock()
            if first { action?() }
        }

        var fired: Bool {
            lock.lock()
            defer { lock.unlock() }
            return didFire
        }
    }

    /// Two scanners over the same tree must agree. This is the check that would
    /// fail if an abandoned worker from one walk leaked into another, which is
    /// the whole risk in not waiting for them.
    func testTwoScannersAgreeOnTheSameTree() throws {
        let root = try TemporaryTree.make([
            "a/one.txt": "aaaa",
            "b/two.txt": "bb",
        ])
        defer { TemporaryTree.remove(root) }

        let first = TreeScanner(options: quietOptions()).scan(root: root.path)
        let second = TreeScanner(options: quietOptions()).scan(root: root.path)

        XCTAssertEqual(first.files, second.files)
        XCTAssertEqual(first.directories, second.directories)
        XCTAssertEqual(first.arena.logical[0], second.arena.logical[0])
        XCTAssertEqual(first.arena.allocated[0], second.arena.allocated[0])
        XCTAssertEqual(second.abandonedWorkers, 0)
    }

    /// Worth stating because it is surprising: a scanner walks once. The totals
    /// and the arena are kept afterwards, which is what lets `rescan` refresh
    /// one folder in place, and it is why the app builds a new scanner for every
    /// new scan rather than reusing this one.
    func testAScannerIsForOneScan() throws {
        let root = try TemporaryTree.make(["a/one.txt": "aaaa"])
        defer { TemporaryTree.remove(root) }

        let scanner = TreeScanner(options: quietOptions())
        let first = scanner.scan(root: root.path)
        let again = scanner.scan(root: root.path)
        XCTAssertEqual(first.files, 1)
        XCTAssertEqual(again.files, 2, "counts accumulate; the instance is spent")
    }

    func testSkippingCanBeTurnedBackOff() {
        let scanner = TreeScanner(options: quietOptions())
        XCTAssertFalse(scanner.isSkippingCloudFolders)
        scanner.setSkipCloudFolders(true)
        XCTAssertTrue(scanner.isSkippingCloudFolders)
        scanner.setSkipCloudFolders(false)
        XCTAssertFalse(scanner.isSkippingCloudFolders)
    }

    /// A scan starts from the setting, so the offer made mid-scan is not
    /// inherited by the next one.
    func testEachScanStartsFromTheOption() throws {
        let root = try TemporaryTree.make(["a.txt": "x"])
        defer { TemporaryTree.remove(root) }

        let scanner = TreeScanner(options: quietOptions())
        scanner.setSkipCloudFolders(true)
        _ = scanner.scan(root: root.path)
        XCTAssertFalse(
            scanner.isSkippingCloudFolders,
            "the option was off, so the new walk should not still be skipping")
    }

    func testMissingRootIsAnErrorRatherThanACrash() {
        let result = TreeScanner(options: quietOptions()).scan(root: "/no/such/path/here")
        XCTAssertEqual(result.errors, 1)
        XCTAssertEqual(result.files, 0)
    }

    private func quietOptions() -> ScanOptions {
        var options = ScanOptions()
        // Small and deterministic: these trees are tiny, and fewer threads makes
        // a failure easier to read.
        options.workers = 4
        return options
    }
}
