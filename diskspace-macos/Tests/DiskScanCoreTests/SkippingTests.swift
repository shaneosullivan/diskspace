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

    // MARK: - Against a real provider, where the machine has one

    /// Everything above declares a root rather than discovering one, which is
    /// the one thing the injection cannot prove. This walks the folder macOS
    /// keeps domains in and lets the scanner find them by attribute, as it does
    /// in the app — so on a machine with a sync service set up, the discovery
    /// path is covered too.
    func testARealProviderIsDiscoveredDuringTheWalkAndSkipped() throws {
        let cloudStorage = NSHomeDirectory() + "/Library/CloudStorage"
        let domains =
            (try? FileManager.default.contentsOfDirectory(atPath: cloudStorage))?
            .filter { !$0.hasPrefix(".") }
            .filter { CloudFolders.isProviderRoot(cloudStorage + "/" + $0) } ?? []
        try XCTSkipIf(domains.isEmpty, "no File Provider domain on this machine")

        var opts = options()
        opts.skipCloudFolders = true  // and no additionalCloudRoots at all
        let result = TreeScanner(options: opts).scan(root: cloudStorage)

        XCTAssertGreaterThanOrEqual(
            result.cloudFoldersSkipped, 1, "the domain should have been found by attribute")
        for domain in domains {
            let node = try XCTUnwrap(node(named: domain, in: result.arena))
            XCTAssertNotEqual(
                result.arena.flags[node] & NodeFlags.isCloudRoot, 0,
                "\(domain) was not recognised while walking")
            XCTAssertNotEqual(
                result.arena.flags[node] & NodeFlags.isCloudUnread, 0,
                "\(domain) should have been left unopened")
            XCTAssertEqual(result.arena.childCount[node], 0)
        }
    }

    /// Scanning a folder that sits *inside* a provider rather than at its top.
    ///
    /// The attribute lives only on the domain's top directory, so such a scan
    /// used to meet nothing that identified it: with skipping switched on the
    /// whole subtree was walked anyway, nothing was labelled, and the setting
    /// appeared to do nothing at all. The scan now asks about the folders above
    /// its root.
    func testScanningInsideAProviderIsStillRecognised() throws {
        let cloudStorage = NSHomeDirectory() + "/Library/CloudStorage"
        let domains =
            (try? FileManager.default.contentsOfDirectory(atPath: cloudStorage))?
            .filter { !$0.hasPrefix(".") }
            .filter { CloudFolders.isProviderRoot(cloudStorage + "/" + $0) } ?? []
        try XCTSkipIf(domains.isEmpty, "no File Provider domain here")
        let domain = domains[0]

        // A folder one level inside the domain, whichever one comes first.
        let inside =
            (try? FileManager.default.contentsOfDirectory(atPath: cloudStorage + "/" + domain))?
            .filter { !$0.hasPrefix(".") } ?? []
        try XCTSkipIf(inside.isEmpty, "the domain is empty")
        let start = cloudStorage + "/" + domain + "/" + inside[0]
        var isDirectory: ObjCBool = false
        _ = FileManager.default.fileExists(atPath: start, isDirectory: &isDirectory)
        try XCTSkipIf(!isDirectory.boolValue, "first entry is not a folder")

        var opts = options()
        opts.skipCloudFolders = true
        let result = TreeScanner(options: opts).scan(root: start)

        XCTAssertEqual(
            result.files, 0, "the setting should apply to a folder inside a provider too")
        XCTAssertEqual(result.cloudFoldersSkipped, 1)
        XCTAssertNotEqual(result.arena.flags[0] & NodeFlags.isCloudUnread, 0)
    }

    // MARK: - Counting providers rather than directories

    /// The count is per provider, not per directory inside one. Leaving one
    /// folder alone once reported "2054 online folders not counted", which is
    /// the number of things inside it and no use to anyone.
    ///
    /// Checked against the ancestor walk directly, because producing several
    /// skipped directories under one already-walked root needs skipping to be
    /// switched on part way through a real walk, and when that lands is up to
    /// the scheduler.
    func testManySkippedDirectoriesUnderOneProviderCountAsOne() throws {
        let root = try TemporaryTree.make([
            "Provider/a/one.txt": "x",
            "Provider/b/two.txt": "y",
            "Provider/c/d/three.txt": "z",
        ])
        defer { TemporaryTree.remove(root) }

        var opts = options()
        opts.additionalCloudRoots = [root.appendingPathComponent("Provider").path]
        let scanner = TreeScanner(options: opts)
        let result = scanner.scan(root: root.path)

        // Every directory inside the provider, as a mid-scan skip would leave
        // them: each one reports itself, and they must fold into one provider.
        let inside = (0..<result.arena.count).filter { node in
            result.arena.flags[node] & NodeFlags.isDir != 0
                && ["a", "b", "c", "d"].contains(result.arena.name(node))
        }
        XCTAssertEqual(inside.count, 4, "the fixture should have four of them")
        for node in inside {
            scanner.noteIncompleteCloudRoot(above: Int32(node))
        }

        XCTAssertEqual(
            scanner.incompleteCloudRoots.count, 1,
            "four directories under one provider is one provider")
    }

    /// What is missing is counted afresh for each walk, or a refresh would
    /// report a provider skipped by the scan before it.
    func testTheCountDoesNotCarryFromOneWalkToTheNext() throws {
        let root = try TemporaryTree.make([
            "local/one.txt": "x",
            "Provider/inside.txt": "y",
        ])
        defer { TemporaryTree.remove(root) }

        var opts = options()
        opts.skipCloudFolders = true
        opts.additionalCloudRoots = [root.appendingPathComponent("Provider").path]
        let scanner = TreeScanner(options: opts)
        let first = scanner.scan(root: root.path)
        XCTAssertEqual(first.cloudFoldersSkipped, 1)

        let local = try XCTUnwrap(node(named: "local", in: scanner.arena))
        let second = scanner.rescan(node: Int32(local))
        XCTAssertEqual(
            second.cloudFoldersSkipped, 0,
            "this walk skipped nothing; the previous walk's answer is not its own")
    }

    /// Refreshing a folder that was skipped un-marks it, because it has just
    /// been read. Left set, the row would go on saying it was never scanned
    /// while showing the bytes the refresh had just counted.
    ///
    /// The mark is put on by hand rather than by a skipping walk: the sequence
    /// this guards against needs skipping switched on part way through and then
    /// off again, and a scanner takes that setting once, at the start.
    func testRefreshingASkippedFolderClearsTheLabel() throws {
        let root = try TemporaryTree.make(["Provider/inside.txt": "hello"])
        defer { TemporaryTree.remove(root) }

        var opts = options()
        opts.additionalCloudRoots = [root.appendingPathComponent("Provider").path]
        let scanner = TreeScanner(options: opts)
        _ = scanner.scan(root: root.path)

        let provider = try XCTUnwrap(node(named: "Provider", in: scanner.arena))
        scanner.arena.flags[provider] |= NodeFlags.isCloudUnread

        let result = scanner.rescan(node: Int32(provider))

        XCTAssertEqual(
            result.arena.flags[provider] & NodeFlags.isCloudUnread, 0,
            "it was just scanned, so saying it was not is false")
        XCTAssertGreaterThan(result.arena.childCount[provider], 0)
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
