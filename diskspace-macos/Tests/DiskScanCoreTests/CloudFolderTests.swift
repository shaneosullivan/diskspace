import XCTest

@testable import DiskScanCore

/// Recognising folders whose contents are not on this disk.
///
/// These are the parts that can be decided from a path or a flag, so they can be
/// tested without a sync provider installed. The parts that cannot — whether a
/// real File Provider domain carries the extended attribute — are checked
/// against the machine's own domains where it has any, and skipped otherwise.
final class CloudFolderTests: XCTestCase {

    // MARK: - Which directories are worth asking about

    func testOnlyChildrenOfCloudStorageAreCandidates() {
        XCTAssertTrue(mayBeRoot("/Users/me/Library/CloudStorage/Dropbox"))
        XCTAssertTrue(mayBeRoot("/Users/me/Library/CloudStorage/iCloud Drive"))
        // The container itself is not a domain, only the things inside it.
        XCTAssertFalse(mayBeRoot("/Users/me/Library/CloudStorage"))
        XCTAssertFalse(mayBeRoot("/Users/me/Library"))
        // A grandchild is inside a domain, not the top of one.
        XCTAssertFalse(mayBeRoot("/Users/me/Library/CloudStorage/Dropbox/Documents"))
    }

    /// This is a filter on what is worth one `getxattr`, not the answer itself,
    /// so it is deliberately loose: any folder called CloudStorage makes its
    /// children candidates, wherever it sits. The attribute is what actually
    /// decides, and it says no for these — the only cost of the looseness is a
    /// wasted syscall on a machine with a folder of that name somewhere odd.
    func testCandidacyIsAFilterRatherThanTheAnswer() {
        XCTAssertTrue(mayBeRoot("/Users/me/CloudStorage/Dropbox"))
        XCTAssertFalse(CloudFolders.isProviderRoot("/Users/me/CloudStorage/Dropbox"))
    }

    func testCandidateTestHandlesDegeneratePaths() {
        XCTAssertFalse(mayBeRoot(""))
        XCTAssertFalse(mayBeRoot("/"))
        XCTAssertFalse(mayBeRoot("relative/path"))
        XCTAssertFalse(mayBeRoot("/CloudStorage"))
        // No parent component at all above the candidate.
        XCTAssertFalse(mayBeRoot("/Dropbox"))
    }

    private func mayBeRoot(_ path: String) -> Bool {
        CloudFolders.mayBeProviderRoot(path: Array(path.utf8)[...])
    }

    // MARK: - The dataless flag

    func testDatalessFlagIsReadFromStFlags() {
        // SF_DATALESS, from sys/stat.h.
        XCTAssertTrue(CloudFolders.isDataless(0x4000_0000))
        // Set alongside the flags a provider's files actually carry.
        XCTAssertTrue(CloudFolders.isDataless(0x4000_0000 | 0x0000_0002 | 0x0002_0000))
        XCTAssertFalse(CloudFolders.isDataless(0))
        // UF_COMPRESSED and SF_IMMUTABLE on their own are not it: an ordinary
        // compressed file is entirely present on this disk.
        XCTAssertFalse(CloudFolders.isDataless(0x0000_0020))
        XCTAssertFalse(CloudFolders.isDataless(0x0002_0000))
    }

    // MARK: - Knowing a descendant of a root when we see one

    func testRegistryMatchesRootsAndTheirDescendants() {
        let registry = CloudRootRegistry()
        registry.add(Array("/Users/me/Library/CloudStorage/Dropbox".utf8))

        XCTAssertTrue(contains(registry, "/Users/me/Library/CloudStorage/Dropbox"))
        XCTAssertTrue(contains(registry, "/Users/me/Library/CloudStorage/Dropbox/Docs"))
        XCTAssertTrue(contains(registry, "/Users/me/Library/CloudStorage/Dropbox/a/b/c"))
    }

    /// The bug this guards: a prefix is only a parent when the next byte is a
    /// separator, or "Dropbox Archive" — a perfectly ordinary local folder —
    /// would be skipped along with the provider's.
    func testRegistryDoesNotMatchASiblingSharingAPrefix() {
        let registry = CloudRootRegistry()
        registry.add(Array("/Users/me/Library/CloudStorage/Dropbox".utf8))

        XCTAssertFalse(contains(registry, "/Users/me/Library/CloudStorage/Dropbox Archive"))
        XCTAssertFalse(contains(registry, "/Users/me/Library/CloudStorage/DropboxOld/x"))
        XCTAssertFalse(contains(registry, "/Users/me/Library/CloudStorage"))
        XCTAssertFalse(contains(registry, "/Users/me"))
        XCTAssertFalse(contains(registry, "/"))
        XCTAssertFalse(contains(registry, ""))
    }

    func testRegistryHandlesSeveralRoots() {
        let registry = CloudRootRegistry()
        registry.add(Array("/a/Library/CloudStorage/One".utf8))
        registry.add(Array("/a/Library/CloudStorage/Two".utf8))

        XCTAssertTrue(contains(registry, "/a/Library/CloudStorage/One/x"))
        XCTAssertTrue(contains(registry, "/a/Library/CloudStorage/Two/y"))
        XCTAssertFalse(contains(registry, "/a/Library/CloudStorage/Three/z"))
    }

    func testRegistryIgnoresARepeatedRoot() {
        let registry = CloudRootRegistry()
        let root = Array("/a/Library/CloudStorage/One".utf8)
        registry.add(root)
        registry.add(root)
        XCTAssertTrue(contains(registry, "/a/Library/CloudStorage/One/x"))
    }

    /// Legacy iCloud Drive predates File Provider domains and carries no
    /// attribute, so it is known by name from the start.
    func testLegacyICloudIsKnownWithoutBeingDiscovered() {
        let registry = CloudRootRegistry()
        let mobileDocuments = NSHomeDirectory() + "/Library/Mobile Documents"
        XCTAssertTrue(contains(registry, mobileDocuments))
        XCTAssertTrue(contains(registry, mobileDocuments + "/com~apple~TextEdit"))
    }

    func testRegistryIsSafeFromSeveralThreadsAtOnce() {
        let registry = CloudRootRegistry()
        let group = DispatchGroup()
        for i in 0..<8 {
            DispatchQueue.global().async(group: group) {
                for j in 0..<200 {
                    registry.add(Array("/a/Library/CloudStorage/p\(i)-\(j)".utf8))
                    _ = registry.contains(
                        Array("/a/Library/CloudStorage/p\(i)-\(j)/child".utf8)[...])
                }
            }
        }
        XCTAssertEqual(group.wait(timeout: .now() + 30), .success)
        XCTAssertTrue(contains(registry, "/a/Library/CloudStorage/p3-100/child"))
    }

    private func contains(_ registry: CloudRootRegistry, _ path: String) -> Bool {
        registry.contains(Array(path.utf8)[...])
    }

    // MARK: - Against this machine, where it has a domain to look at

    /// The extended attribute is the whole basis of the detection, so where the
    /// machine running the tests has a real domain, check it really is there —
    /// and, just as importantly, that an ordinary folder does not have it.
    func testRealProviderDomainCarriesTheAttribute() throws {
        let cloudStorage = NSHomeDirectory() + "/Library/CloudStorage"
        let domains =
            (try? FileManager.default.contentsOfDirectory(atPath: cloudStorage))?
            .filter { !$0.hasPrefix(".") } ?? []
        try XCTSkipIf(domains.isEmpty, "no File Provider domain on this machine")

        for domain in domains {
            XCTAssertTrue(
                CloudFolders.isProviderRoot(cloudStorage + "/" + domain),
                "\(domain) should be recognised as a provider root")
        }
        XCTAssertFalse(CloudFolders.isProviderRoot(NSHomeDirectory()))
        XCTAssertFalse(CloudFolders.isProviderRoot(cloudStorage))
    }

    func testAnOrdinaryDirectoryIsNotAProviderRoot() throws {
        let dir = try TemporaryTree.make(["a/b": "hello"])
        defer { TemporaryTree.remove(dir) }
        XCTAssertFalse(CloudFolders.isProviderRoot(dir.path))
        XCTAssertFalse(CloudFolders.isProviderRoot(dir.path + "/a"))
    }

    func testMissingPathIsNotAProviderRoot() {
        XCTAssertFalse(CloudFolders.isProviderRoot("/no/such/path/anywhere"))
    }
}
