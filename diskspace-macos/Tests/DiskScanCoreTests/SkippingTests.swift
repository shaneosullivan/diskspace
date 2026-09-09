import XCTest

@testable import DiskScanCore

/// Leaving a sync provider's folder unwalked.
///
/// A real File Provider domain cannot be created by a test, so these declare an
/// ordinary directory to be a provider root through `additionalCloudRoots` and
/// check what the scan then does with it. That is the whole point of the
/// injection: without it nothing here could reach the code that decides whether
/// an entire tree is counted.
final class SkippingTests: XCTestCase {

    /// The tree used throughout: a folder standing in for a provider's, with
    /// real files inside it, beside an ordinary local folder.
    private func makeTree() throws -> URL {
        try TemporaryTree.make([
            "local/keep.txt": String(repeating: "k", count: 100),
            "local/deeper/also.txt": String(repeating: "a", count: 50),
            "Provider/inside.txt": String(repeating: "p", count: 1000),
            "Provider/nested/deep.txt": String(repeating: "d", count: 2000),
        ])
    }

    func testSkippingLeavesTheProvidersContentsOutOfTheTotals() throws {
        let root = try makeTree()
        defer { TemporaryTree.remove(root) }

        var opts = options()
        opts.skipCloudFolders = true
        opts.additionalCloudRoots = [root.appendingPathComponent("Provider").path]
        let skipped = TreeScanner(options: opts).scan(root: root.path)

        // The two local files, and neither of the provider's.
        XCTAssertEqual(skipped.files, 2)
        XCTAssertEqual(skipped.cloudFoldersSkipped, 1)

        var fileBytes: Int64 = 0
        for i in 0..<skipped.arena.count
        where skipped.arena.flags[i] & NodeFlags.isDir == 0 {
            fileBytes += skipped.arena.logical[i]
        }
        XCTAssertEqual(fileBytes, 150, "only the local files: 100 + 50")
    }

    /// The same tree with skipping off, to show the difference is the feature
    /// and not the fixture.
    func testTheSameTreeIsCountedInFullWhenNotSkipping() throws {
        let root = try makeTree()
        defer { TemporaryTree.remove(root) }

        var opts = options()
        opts.additionalCloudRoots = [root.appendingPathComponent("Provider").path]
        let full = TreeScanner(options: opts).scan(root: root.path)

        XCTAssertEqual(full.files, 4)
        XCTAssertEqual(full.cloudFoldersSkipped, 0)

        var fileBytes: Int64 = 0
        for i in 0..<full.arena.count where full.arena.flags[i] & NodeFlags.isDir == 0 {
            fileBytes += full.arena.logical[i]
        }
        XCTAssertEqual(fileBytes, 3150, "100 + 50 + 1000 + 2000")
    }

    func testASkippedFolderIsMarkedAsBothCloudAndUnread() throws {
        let root = try makeTree()
        defer { TemporaryTree.remove(root) }

        var opts = options()
        opts.skipCloudFolders = true
        opts.additionalCloudRoots = [root.appendingPathComponent("Provider").path]
        let result = TreeScanner(options: opts).scan(root: root.path)

        let provider = try XCTUnwrap(node(named: "Provider", in: result.arena))
        XCTAssertNotEqual(result.arena.flags[provider] & NodeFlags.isCloudRoot, 0)
        XCTAssertNotEqual(
            result.arena.flags[provider] & NodeFlags.isCloudUnread, 0,
            "it was not opened, and the label depends on knowing that")
        XCTAssertEqual(result.arena.childCount[provider], 0)
    }

    /// The distinction the `isCloudUnread` flag exists for: a provider folder
    /// that *was* walked must not claim it was skipped, even though a walked
    /// folder can also end up with nothing in it.
    func testAWalkedProviderFolderIsNotMarkedUnread() throws {
        let root = try TemporaryTree.make([
            "Provider/inside.txt": "x",
            "Empty/": "",
        ])
        defer { TemporaryTree.remove(root) }

        var opts = options()
        opts.additionalCloudRoots = [root.appendingPathComponent("Provider").path]
        let result = TreeScanner(options: opts).scan(root: root.path)

        let provider = try XCTUnwrap(node(named: "Provider", in: result.arena))
        XCTAssertNotEqual(result.arena.flags[provider] & NodeFlags.isCloudRoot, 0)
        XCTAssertEqual(
            result.arena.flags[provider] & NodeFlags.isCloudUnread, 0,
            "it was walked, so saying it was not scanned would be false")

        // And an ordinary empty folder is not confused with either.
        let empty = try XCTUnwrap(node(named: "Empty", in: result.arena))
        XCTAssertEqual(result.arena.flags[empty] & NodeFlags.isCloudRoot, 0)
        XCTAssertEqual(result.arena.childCount[empty], 0)
    }

    /// Descendants, not just the root: switching skipping on has to stop the
    /// subdirectories of a provider folder as well as the folder itself.
    func testEverythingUnderAProviderRootIsSkippedToo() throws {
        let root = try TemporaryTree.make([
            "Provider/a/b/c/buried.txt": String(repeating: "z", count: 500)
        ])
        defer { TemporaryTree.remove(root) }

        var opts = options()
        opts.skipCloudFolders = true
        opts.additionalCloudRoots = [root.appendingPathComponent("Provider").path]
        let result = TreeScanner(options: opts).scan(root: root.path)

        XCTAssertEqual(result.files, 0)
        for i in 0..<result.arena.count {
            XCTAssertNotEqual(
                result.arena.name(i), "buried.txt", "nothing below the root should be reached")
        }
    }

    /// A sibling whose name merely starts the same way is an ordinary folder and
    /// must still be counted.
    func testASiblingSharingAPrefixIsNotSkipped() throws {
        let root = try TemporaryTree.make([
            "Provider/inside.txt": "x",
            "Provider Archive/kept.txt": String(repeating: "k", count: 42),
        ])
        defer { TemporaryTree.remove(root) }

        var opts = options()
        opts.skipCloudFolders = true
        opts.additionalCloudRoots = [root.appendingPathComponent("Provider").path]
        let result = TreeScanner(options: opts).scan(root: root.path)

        XCTAssertEqual(result.files, 1, "the archive's file, and not the provider's")
        XCTAssertEqual(result.cloudFoldersSkipped, 1)
        let archive = try XCTUnwrap(node(named: "Provider Archive", in: result.arena))
        XCTAssertEqual(result.arena.flags[archive] & NodeFlags.isCloudRoot, 0)
        XCTAssertEqual(result.arena.childCount[archive], 1)
    }

    /// Scanning a provider's folder directly. The candidacy filter alone says no
    /// for such a path, so the scan root is checked as a special case — and a
    /// refresh seeds the walk somewhere other than node zero, which is why the
    /// check is against the seed rather than against zero.
    func testAProviderFolderScannedDirectlyIsStillRecognised() throws {
        let root = try makeTree()
        defer { TemporaryTree.remove(root) }
        let provider = root.appendingPathComponent("Provider").path

        var opts = options()
        opts.skipCloudFolders = true
        opts.additionalCloudRoots = [provider]
        let result = TreeScanner(options: opts).scan(root: provider)

        XCTAssertEqual(result.files, 0)
        XCTAssertEqual(result.cloudFoldersSkipped, 1)
        XCTAssertNotEqual(result.arena.flags[0] & NodeFlags.isCloudRoot, 0)
    }

    /// Skipping is a decision about this scan. A refresh of one folder is a new
    /// scan, and must go back to what the settings say — otherwise one press of
    /// "Skip them" quietly changes every refresh for the life of the scanner.
    func testARefreshGoesBackToTheSetting() throws {
        let root = try makeTree()
        defer { TemporaryTree.remove(root) }

        var opts = options()
        opts.additionalCloudRoots = [root.appendingPathComponent("Provider").path]
        let scanner = TreeScanner(options: opts)
        _ = scanner.scan(root: root.path)

        scanner.setSkipCloudFolders(true)  // as the banner's button does
        XCTAssertTrue(scanner.isSkippingCloudFolders)

        let provider = try XCTUnwrap(node(named: "Provider", in: scanner.arena))
        _ = scanner.rescan(node: Int32(provider))
        XCTAssertFalse(
            scanner.isSkippingCloudFolders,
            "the option was off, so a refresh should not still be skipping")
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
}
