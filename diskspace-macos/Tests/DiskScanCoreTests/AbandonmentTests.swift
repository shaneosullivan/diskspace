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
        XCTAssertFalse(
            TreeScanner.onlyWaitingOnProviders(skipping: false, drained: true, held: [true]))
        XCTAssertTrue(
            TreeScanner.onlyWaitingOnProviders(skipping: true, drained: true, held: [true]))
    }

    /// If directories are still queued, an idle worker is about to pick one up
    /// and the scan is going somewhere.
    func testNothingIsAbandonedWhileWorkIsStillQueued() {
        XCTAssertFalse(
            TreeScanner.onlyWaitingOnProviders(skipping: true, drained: false, held: [true]))
    }

    /// The one that matters most: a single worker on an ordinary local directory
    /// vetoes the whole thing, because cutting it off would silently drop
    /// whatever that directory contains.
    func testOneWorkerOnALocalDirectoryProtectsTheWholeScan() {
        XCTAssertFalse(
            TreeScanner.onlyWaitingOnProviders(
                skipping: true, drained: true, held: [true, true, false]))
        XCTAssertTrue(
            TreeScanner.onlyWaitingOnProviders(
                skipping: true, drained: true, held: [true, true, true]))
    }

    /// Workers waiting on the queue hold nothing, so they are not listed at all
    /// — and with nobody holding anything there is nothing to abandon.
    func testIdleWorkersAreNotSomethingToAbandon() {
        XCTAssertFalse(
            TreeScanner.onlyWaitingOnProviders(skipping: true, drained: true, held: []))
    }

    // MARK: - How long the condition has to hold
    //
    // The bug this replaced: the wait was measured from the worker's arrival in
    // its directory, which is already old by the time anyone looks, so a single
    // poll could authorise abandoning a scan. What has to be timed is how long
    // the condition itself has held.

    func testAConditionThatHasJustBecomeTrueIsNotEnough() {
        let now = DispatchTime.now().uptimeNanoseconds
        XCTAssertFalse(TreeScanner.heldLongEnough(since: now, now: now, limit: 2))
        XCTAssertFalse(
            TreeScanner.heldLongEnough(since: now &- UInt64(1.9 * 1e9), now: now, limit: 2))
    }

    func testTheConditionMustHaveHeldForTheWholeInterval() {
        let now = DispatchTime.now().uptimeNanoseconds
        XCTAssertTrue(
            TreeScanner.heldLongEnough(since: now &- UInt64(2.1 * 1e9), now: now, limit: 2))
    }

    /// Nil is "the condition is not currently true", which is how the walk
    /// records a poll that broke the run.
    func testAConditionThatIsNotHoldingIsNeverLongEnough() {
        XCTAssertFalse(
            TreeScanner.heldLongEnough(
                since: nil, now: DispatchTime.now().uptimeNanoseconds, limit: 0))
    }

    // MARK: - What abandoning does

    /// The safety argument in one test. A worker carries the epoch it started
    /// with; a walk that has given up moves the epoch on, and the worker's own
    /// check then tells it to drop what it is holding rather than hand a
    /// directory from a dead scan to a live one.
    func testAbandoningMovesTheEpochSoALateWorkerStandsDown() {
        let scanner = TreeScanner(options: options())
        let epochAWorkerWouldHold = scanner.currentEpoch

        scanner.abandon(dirs: DirQueue(seed: 1))

        XCTAssertFalse(
            scanner.isCurrent(epochAWorkerWouldHold),
            "a worker from the abandoned walk must know its walk is over")
    }

    func testAbandoningReleasesWorkersWaitingOnTheQueue() {
        let scanner = TreeScanner(options: options())
        let dirs = DirQueue(seed: 1)
        _ = dirs.take()  // drained, so a further take() would block

        scanner.abandon(dirs: dirs)
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
}
