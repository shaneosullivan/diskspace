import XCTest

@testable import DiskScanCore

/// Taking things out of the tree, and putting them back.
///
/// A pending delete removes an entry's figures from every total; undoing puts
/// them back. The case worth guarding is the ordinary one: delete a file, then
/// delete the folder it was in.
final class RemovalTests: XCTestCase {

    /// Removing a folder must not take out figures already taken out with one of
    /// its children.
    ///
    /// It used to: the walk over a subtree counted every child, including ones
    /// already removed, so a file inside a folder was subtracted twice — once
    /// when it went, once when the folder did. The per-kind file count is not
    /// derived from the arena, so it went negative, and `kindTotals` reports
    /// only counts above zero — the type disappeared from the breakdown
    /// altogether.
    func testRemovingAFolderDoesNotSubtractAChildTwice() throws {
        let root = try TemporaryTree.make([
            "a/b/one.txt": "1",
            "a/b/two.txt": "22",
            "keep.txt": "333",
        ])
        defer { TemporaryTree.remove(root) }

        let scanner = TreeScanner(options: options())
        let result = scanner.scan(root: root.path)
        let kind = try XCTUnwrap(textKind(in: result.arena))
        XCTAssertEqual(count(of: kind, in: scanner), 3, "three .txt files to begin with")

        let one = try XCTUnwrap(node(named: "one.txt", in: result.arena))
        scanner.remove(node: Int32(one))
        XCTAssertEqual(count(of: kind, in: scanner), 2, "one of them is gone")

        let folder = try XCTUnwrap(node(named: "b", in: result.arena))
        scanner.remove(node: Int32(folder))
        XCTAssertEqual(
            count(of: kind, in: scanner), 1,
            "keep.txt is still there; one.txt must not be subtracted a second time")
    }

    /// And the figures come back, which is what makes a pending delete
    /// reversible.
    func testRestoringPutsTheFiguresBack() throws {
        let root = try TemporaryTree.make([
            "a/b/one.txt": "1",
            "a/b/two.txt": "22",
            "keep.txt": "333",
        ])
        defer { TemporaryTree.remove(root) }

        let scanner = TreeScanner(options: options())
        let result = scanner.scan(root: root.path)
        let kind = try XCTUnwrap(textKind(in: result.arena))
        let before = scanner.arena.allocated[0]

        let one = try XCTUnwrap(node(named: "one.txt", in: result.arena))
        let folder = try XCTUnwrap(node(named: "b", in: result.arena))
        let removedChild = scanner.remove(node: Int32(one))
        let removedFolder = scanner.remove(node: Int32(folder))

        scanner.restore(removedFolder)
        scanner.restore(removedChild)

        XCTAssertEqual(count(of: kind, in: scanner), 3)
        XCTAssertEqual(scanner.arena.allocated[0], before)
    }

    // MARK: -

    private func options() -> ScanOptions {
        var options = ScanOptions()
        options.workers = 4
        return options
    }

    private func node(named name: String, in arena: NodeArena) -> Int? {
        (0..<arena.count).first { arena.name($0) == name }
    }

    private func textKind(in arena: NodeArena) -> Int16? {
        node(named: "keep.txt", in: arena).map { arena.kind[$0] }
    }

    private func count(of kind: Int16, in scanner: TreeScanner) -> Int {
        scanner.kindTotals().first { $0.kind == kind }?.count ?? 0
    }
}
