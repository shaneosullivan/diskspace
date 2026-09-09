import XCTest

@testable import DiskScanCore

/// Deciding to stop waiting for a worker.
///
/// A thread inside a syscall cannot be killed, so a scan stuck on a sync
/// provider can only be ended by walking away from it. That is safe exactly when
/// two things hold: nothing is being given up except a provider's folder, and
/// the worker left behind can no longer reach the scan that comes after it.
/// These check both, since the situation itself cannot be staged on a build
/// machine — nothing here blocks a directory read for seconds on demand.
final class AbandonmentTests: XCTestCase {

    // MARK: - When a worker may be left behind

    /// The first guard. Someone who never asked to skip provider folders must
    /// never have their scan cut short, however slow it is.
    func testNothingIsAbandonedUnlessSkippingWasAskedFor() {
        let scanner = TreeScanner(options: options())
        scanner.prepareWorkerTableForTesting(count: 2)
        scanner.placeWorkerForTesting(0, inCloud: true, secondsAgo: 60)
        let dirs = drainedQueue()

        XCTAssertNil(
            scanner.stuckOnProviderSince(dirs: dirs, workers: []),
            "skipping is off, so waiting is the only correct thing to do")

        scanner.setSkipCloudFolders(true)
        XCTAssertNotNil(scanner.stuckOnProviderSince(dirs: dirs, workers: []))
    }

    /// The second guard. If the queue still holds directories, an idle worker
    /// will pick them up and the scan is going somewhere — abandoning now would
    /// throw away work that was about to happen.
    func testNothingIsAbandonedWhileWorkIsStillQueued() {
        let scanner = TreeScanner(options: options())
        scanner.setSkipCloudFolders(true)
        scanner.prepareWorkerTableForTesting(count: 2)
        scanner.placeWorkerForTesting(0, inCloud: true, secondsAgo: 60)

        let dirs = DirQueue(seed: 1)  // seed still queued
        XCTAssertFalse(dirs.isDrained)
        XCTAssertNil(scanner.stuckOnProviderSince(dirs: dirs, workers: []))

        _ = dirs.take()
        XCTAssertTrue(dirs.isDrained)
        XCTAssertNotNil(scanner.stuckOnProviderSince(dirs: dirs, workers: []))
    }

    /// The third and most important guard. One worker busy with an ordinary
    /// local directory means the scan is making progress, and cutting it off
    /// would silently drop whatever that directory contains.
    func testOneWorkerOnALocalDirectoryProtectsTheWholeScan() {
        let scanner = TreeScanner(options: options())
        scanner.setSkipCloudFolders(true)
        scanner.prepareWorkerTableForTesting(count: 3)
        scanner.placeWorkerForTesting(0, inCloud: true, secondsAgo: 60)
        scanner.placeWorkerForTesting(1, inCloud: true, secondsAgo: 60)
        scanner.placeWorkerForTesting(2, inCloud: false, secondsAgo: 0.1)

        XCTAssertNil(
            scanner.stuckOnProviderSince(dirs: drainedQueue(), workers: []),
            "a worker on a local directory means the scan is still going")
    }

    func testIdleWorkersDoNotCountAsStuck() {
        let scanner = TreeScanner(options: options())
        scanner.setSkipCloudFolders(true)
        scanner.prepareWorkerTableForTesting(count: 4)
        // Nobody placed: every worker is waiting on the queue, holding nothing.
        XCTAssertNil(
            scanner.stuckOnProviderSince(dirs: drainedQueue(), workers: []),
            "with nothing in hand there is nothing to abandon")
    }

    /// The age reported is the oldest worker's, since that is what the wait is
    /// measured against.
    func testTheOldestStuckWorkerSetsTheClock() {
        let scanner = TreeScanner(options: options())
        scanner.setSkipCloudFolders(true)
        scanner.prepareWorkerTableForTesting(count: 3)
        scanner.placeWorkerForTesting(0, inCloud: true, secondsAgo: 1)
        scanner.placeWorkerForTesting(1, inCloud: true, secondsAgo: 30)
        scanner.placeWorkerForTesting(2, inCloud: true, secondsAgo: 5)

        let since = try? XCTUnwrap(scanner.stuckOnProviderSince(dirs: drainedQueue(), workers: []))
        let waited = Double(DispatchTime.now().uptimeNanoseconds &- (since ?? 0)) / 1e9
        XCTAssertGreaterThan(waited, 25, "the longest wait is the one that counts")
    }

    // MARK: - What abandoning actually does

    /// The whole safety argument. A worker carries the epoch it started with; a
    /// walk that has given up on it moves the epoch on, and the worker's own
    /// check then tells it to drop what it is holding rather than hand a
    /// directory from a dead scan to a live one.
    func testAbandoningMovesTheEpochSoALateWorkerStandsDown() {
        let scanner = TreeScanner(options: options())
        let epoch = scanner.currentEpoch
        XCTAssertTrue(scanner.isCurrent(epoch))

        scanner.abandon(workers: [], dirs: DirQueue(seed: 1))

        XCTAssertFalse(
            scanner.isCurrent(epoch),
            "a worker from the abandoned walk must know its walk is over")
        XCTAssertTrue(scanner.isCurrent(scanner.currentEpoch))
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
        XCTAssertNotEqual(scanner.currentEpoch, before)
        XCTAssertFalse(scanner.isCurrent(before))
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
        XCTAssertEqual(result.files, 3)
    }

    // MARK: -

    private func options() -> ScanOptions {
        var options = ScanOptions()
        options.workers = 4
        return options
    }

    private func drainedQueue() -> DirQueue {
        let dirs = DirQueue(seed: 1)
        _ = dirs.take()
        return dirs
    }
}
