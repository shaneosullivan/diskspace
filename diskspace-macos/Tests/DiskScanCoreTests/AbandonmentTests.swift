import XCTest

@testable import DiskScanCore

/// Deciding to stop waiting for a worker.
///
/// A thread inside a syscall cannot be killed, so a scan stuck on a sync
/// provider can only be ended by walking away from it. That is safe exactly when
/// two things hold: nothing is given up except a provider's folder, and the
/// worker left behind can no longer reach the scan that comes after it.
final class AbandonmentTests: XCTestCase {

    // MARK: - When a worker may be left behind
    //
    // The decision is a function of values, so these are about the rule itself
    // rather than about a scanner arranged to look a certain way.

    /// Someone who never asked to skip provider folders must never have their
    /// scan cut short, however slow it is.
    func testNothingIsAbandonedUnlessSkippingWasAskedFor() {
        let stuck = [(inCloud: true, since: secondsAgo(60))]
        XCTAssertNil(TreeScanner.stuckSince(skipping: false, drained: true, held: stuck))
        XCTAssertNotNil(TreeScanner.stuckSince(skipping: true, drained: true, held: stuck))
    }

    /// If directories are still queued, an idle worker is about to pick one up
    /// and the scan is going somewhere.
    func testNothingIsAbandonedWhileWorkIsStillQueued() {
        let stuck = [(inCloud: true, since: secondsAgo(60))]
        XCTAssertNil(TreeScanner.stuckSince(skipping: true, drained: false, held: stuck))
        XCTAssertNotNil(TreeScanner.stuckSince(skipping: true, drained: true, held: stuck))
    }

    /// The one that matters most: a single worker on an ordinary local directory
    /// vetoes the whole thing, because cutting it off would silently drop
    /// whatever that directory contains.
    func testOneWorkerOnALocalDirectoryProtectsTheWholeScan() {
        let held = [
            (inCloud: true, since: secondsAgo(60)),
            (inCloud: true, since: secondsAgo(60)),
            (inCloud: false, since: secondsAgo(0.1)),
        ]
        XCTAssertNil(TreeScanner.stuckSince(skipping: true, drained: true, held: held))
    }

    /// Workers waiting on the queue hold nothing, so they are not in `held` at
    /// all — and with nobody holding anything there is nothing to abandon.
    func testIdleWorkersAreNotSomethingToAbandon() {
        XCTAssertNil(TreeScanner.stuckSince(skipping: true, drained: true, held: []))
    }

    func testTheOldestStuckWorkerSetsTheClock() throws {
        let held = [
            (inCloud: true, since: secondsAgo(1)),
            (inCloud: true, since: secondsAgo(30)),
            (inCloud: true, since: secondsAgo(5)),
        ]
        let since = try XCTUnwrap(
            TreeScanner.stuckSince(skipping: true, drained: true, held: held))
        let waited = Double(DispatchTime.now().uptimeNanoseconds &- since) / 1e9
        XCTAssertGreaterThan(waited, 25, "the longest wait is the one that counts")
        XCTAssertLessThan(waited, 40)
    }

    // MARK: - What abandoning does

    /// The safety argument in one test. A worker carries the epoch it started
    /// with; a walk that has given up moves the epoch on, and the worker's own
    /// check then tells it to drop what it is holding rather than hand a
    /// directory from a dead scan to a live one.
    func testAbandoningMovesTheEpochSoALateWorkerStandsDown() {
        let scanner = TreeScanner(options: options())
        let epochAWorkerWouldHold = scanner.currentEpoch

        scanner.abandon(workers: [], dirs: DirQueue(seed: 1))

        XCTAssertFalse(
            scanner.isCurrent(epochAWorkerWouldHold),
            "a worker from the abandoned walk must know its walk is over")
    }

    func testAbandoningReleasesWorkersWaitingOnTheQueue() {
        let scanner = TreeScanner(options: options())
        let dirs = DirQueue(seed: 1)
        _ = dirs.take()  // drained, so a further take() would block

        scanner.abandon(workers: [], dirs: dirs)
        XCTAssertNil(dirs.take(), "an idle worker should be let go, not left waiting")
    }

    /// A fresh walk starts a fresh epoch, which is what stops a straggler from a
    /// previous walk being mistaken for one of its own.
    func testEachWalkGetsItsOwnEpoch() throws {
        let root = try TemporaryTree.make(["a.txt": "x"])
        defer { TemporaryTree.remove(root) }

        let scanner = TreeScanner(options: options())
        let before = scanner.currentEpoch
        _ = scanner.scan(root: root.path)
        XCTAssertFalse(
            scanner.isCurrent(before), "the walk that has ended is no longer current")
    }

    /// An ordinary scan leaves nobody behind, and says so.
    func testAnOrdinaryScanAbandonsNobody() throws {
        let root = try TemporaryTree.make([
            "a/one.txt": "x", "b/two.txt": "yy", "c/d/three.txt": "zzz",
        ])
        defer { TemporaryTree.remove(root) }

        var opts = options()
        opts.skipCloudFolders = true
        let result = TreeScanner(options: opts).scan(root: root.path)
        XCTAssertEqual(result.abandonedWorkers, 0)
        XCTAssertEqual(result.cloudFoldersSkipped, 0)
        XCTAssertEqual(result.files, 3)
    }

    // MARK: -

    private func options() -> ScanOptions {
        var options = ScanOptions()
        options.workers = 4
        return options
    }

    private func secondsAgo(_ seconds: Double) -> UInt64 {
        DispatchTime.now().uptimeNanoseconds &- UInt64(seconds * 1e9)
    }
}
