import XCTest

@testable import DiskScanCore

/// The queues the walk is built on, and the properties abandoning a worker
/// depends on.
final class QueueTests: XCTestCase {

    // MARK: - DirQueue

    func testSeedIsHandedOutOnce() {
        let q = DirQueue(seed: 7)
        XCTAssertEqual(q.take(), 7)
        // Nothing left to give, and the walk is not over until it is applied.
        XCTAssertTrue(q.isDrained)
    }

    func testDrainedIsAboutWhatIsQueuedNotWhatIsInFlight() {
        let q = DirQueue(seed: 1)
        XCTAssertFalse(q.isDrained, "the seed is still queued")
        _ = q.take()
        XCTAssertTrue(q.isDrained, "taken, so nothing is left to hand out")
        q.push([2, 3])
        XCTAssertFalse(q.isDrained)
    }

    /// The property abandoning relies on: with the queue drained, an idle worker
    /// has nothing to pick up, so stuck workers really are all that is left.
    func testDrainedGoesBackToFalseWhenChildrenArrive() {
        let q = DirQueue(seed: 1)
        _ = q.take()
        XCTAssertTrue(q.isDrained)
        q.push([2])
        XCTAssertFalse(q.isDrained)
        _ = q.take()
        XCTAssertTrue(q.isDrained)
    }

    func testWalkEndsWhenEveryDirectoryHasBeenApplied() {
        let q = DirQueue(seed: 1)
        XCTAssertEqual(q.take(), 1)
        q.push([2, 3])
        q.completed()  // the seed
        XCTAssertNotNil(q.take())
        XCTAssertNotNil(q.take())
        q.completed()
        q.completed()
        // Everything discovered has now been applied, so workers are released.
        XCTAssertNil(q.take())
    }

    func testCancelReleasesAWaitingWorker() {
        let q = DirQueue(seed: 1)
        _ = q.take()  // drain it, so the next take() has to wait

        let waiting = expectation(description: "worker released")
        DispatchQueue.global().async {
            XCTAssertNil(q.take())
            waiting.fulfill()
        }
        // Give the thread time to actually be waiting, then cancel.
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.05) { q.cancel() }
        wait(for: [waiting], timeout: 5)
    }

    func testCancelIsWhatAbandoningUsesToReleaseIdleWorkers() {
        let q = DirQueue(seed: 1)
        q.cancel()
        XCTAssertNil(q.take())
        XCTAssertNil(q.take(), "cancelling is permanent for this walk")
    }

    // MARK: - BatchQueue

    func testBatchesComeBackInOrder() {
        let q = BatchQueue()
        let first = ScanBatch()
        first.reset(parentNode: 11)
        let second = ScanBatch()
        second.reset(parentNode: 22)
        q.push(first)
        q.push(second)
        XCTAssertEqual(q.take()?.parentNode, 11)
        XCTAssertEqual(q.take()?.parentNode, 22)
    }

    func testClosingDrainsWhatIsThereThenStops() {
        let q = BatchQueue()
        let batch = ScanBatch()
        batch.reset(parentNode: 5)
        q.push(batch)
        q.close()
        XCTAssertEqual(q.take()?.parentNode, 5, "what was pushed is still delivered")
        XCTAssertNil(q.take())
    }

    /// What an abandoned worker does when it finally wakes: it pushes into the
    /// queue of its own walk, which by then is closed and unread. That has to be
    /// harmless rather than blocking it forever or losing the applier.
    func testPushingIntoAClosedQueueIsHarmless() {
        let q = BatchQueue()
        q.close()
        let late = ScanBatch()
        late.reset(parentNode: 99)

        let pushed = expectation(description: "push returned")
        DispatchQueue.global().async {
            q.push(late)
            pushed.fulfill()
        }
        wait(for: [pushed], timeout: 5)
    }

    func testAFullQueuePushesBackRatherThanGrowing() {
        let q = BatchQueue(capacity: 2)
        for i in 0..<2 {
            let b = ScanBatch()
            b.reset(parentNode: Int32(i))
            q.push(b)
        }
        let finished = DispatchSemaphore(value: 0)
        DispatchQueue.global().async {
            let b = ScanBatch()
            b.reset(parentNode: 2)
            q.push(b)  // waits for room
            finished.signal()
        }
        // It should still be waiting; draining one is what lets it through.
        XCTAssertEqual(
            finished.wait(timeout: .now() + 0.2), .timedOut,
            "a full queue should hold the producer, not grow")
        _ = q.take()
        XCTAssertEqual(finished.wait(timeout: .now() + 5), .success)
        XCTAssertGreaterThan(q.blockedPushes, 0)
    }
}
