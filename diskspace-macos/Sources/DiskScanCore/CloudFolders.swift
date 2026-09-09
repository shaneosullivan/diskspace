import Darwin
import Foundation

/// Folders whose contents live somewhere else.
///
/// A sync provider — iCloud Drive, Dropbox, OneDrive, or any app that registers a
/// File Provider domain — presents a tree under `~/Library/CloudStorage` that
/// looks like ordinary local directories and mostly is not. Files in it can be
/// *dataless*: full size on the server, nothing on this disk.
///
/// None of the usual ways of spotting a foreign filesystem work here, which was
/// worth establishing before writing any of this. Measured against a real
/// provider domain, the tree reports the same `st_dev` as the home folder, no
/// entry of its own in the mount table, `apfs` from `statfs`, and `MNT_LOCAL`
/// set. It is synthesised in place on the local volume. Two things do give it
/// away, and they are what this file uses.
public enum CloudFolders {
    /// `SF_DATALESS`, from sys/stat.h: the file's contents are not here.
    ///
    /// Such a file's allocated size is already zero, so the app's default figure
    /// is right without any help. Its *logical* size is not — that is the size on
    /// the provider's server, and counting it makes a folder look like it is
    /// holding gigabytes that no disk anywhere is holding.
    public static let datalessFlag: UInt32 = 0x4000_0000

    @inline(__always)
    public static func isDataless(_ stFlags: UInt32) -> Bool {
        stFlags & datalessFlag != 0
    }

    /// Whether `path` is the root of a File Provider domain.
    ///
    /// macOS hangs an extended attribute naming the provider on the domain's
    /// top directory, and on nothing else, so one `getxattr` answers it without
    /// opening anything. That matters: opening or reading a placeholder can ask
    /// the provider to fetch the file, which is the opposite of what a program
    /// measuring a disk should do.
    ///
    /// The attribute is provider-agnostic — the same one identifies iCloud
    /// Drive, Dropbox, OneDrive and anything else that registers a domain.
    public static func isProviderRoot(_ path: String) -> Bool {
        path.withCString { isProviderRoot(cPath: $0) }
    }

    /// The same test against a path the caller already holds as bytes, which is
    /// how the scanner has it — no String need be built per directory.
    @inline(__always)
    public static func isProviderRoot(cPath: UnsafePointer<CChar>) -> Bool {
        getxattr(cPath, domainIDAttribute, nil, 0, 0, XATTR_NOFOLLOW) >= 0
    }

    /// Whether a directory is worth asking about at all.
    ///
    /// `getxattr` on a path is not cheap: measured over 205,856 directories on
    /// this machine it cost 14-20 µs each warm, 31 µs cold. Asked of every
    /// directory in a home folder that is twenty-odd CPU-seconds spent proving
    /// that almost nothing is a provider root.
    ///
    /// macOS puts every File Provider domain in one place, so the question is
    /// only worth asking of a directory sitting directly inside it. The scan root
    /// is asked as well, whatever it is, so that scanning a provider's folder
    /// directly still recognises it.
    public static func mayBeProviderRoot(path: ArraySlice<UInt8>) -> Bool {
        // The parent component, i.e. what lies between the last two separators.
        guard let lastSlash = path.lastIndex(of: 0x2F), lastSlash > path.startIndex else {
            return false
        }
        let parent = path[path.startIndex..<lastSlash]
        guard let prevSlash = parent.lastIndex(of: 0x2F) else { return false }
        let name = parent[parent.index(after: prevSlash)...]
        return name.elementsEqual(cloudStorageComponent)
    }

    private static let cloudStorageComponent = Array("CloudStorage".utf8)

    private static let domainIDAttribute = "com.apple.file-provider-domain-id"

    /// Legacy iCloud Drive, which predates File Provider domains and carries no
    /// such attribute.
    public static var knownRoots: [String] {
        [NSHomeDirectory() + "/Library/Mobile Documents"]
    }
}

/// The provider roots one scan has found, so their descendants can be recognised
/// without asking the filesystem about every directory again.
///
/// A handful of entries at most — one per sync service the user has — so a linear
/// prefix test is cheaper than anything cleverer, and it is only consulted when
/// skipping is switched on.
final class CloudRootRegistry: @unchecked Sendable {
    private let lock = NSLock()
    /// UTF-8, no trailing NUL, matching how the scanner carries paths.
    private var roots: [[UInt8]]

    init(extraRoots: [String] = []) {
        roots = (CloudFolders.knownRoots + extraRoots).map { Array($0.utf8) }
    }

    func add(_ path: [UInt8]) {
        lock.lock()
        defer { lock.unlock() }
        if !roots.contains(path) { roots.append(path) }
    }

    /// True when `path` is itself one of the roots, rather than merely inside
    /// one. A root already known — seeded, or found by attribute earlier in this
    /// scan — is a root without being asked again.
    func isRoot(_ path: ArraySlice<UInt8>) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return roots.contains { $0.count == path.count && $0.elementsEqual(path) }
    }

    /// True when `path` is one of the roots or lies underneath one.
    ///
    /// The length test before the prefix comparison is what keeps
    /// "/Users/me/Dropbox Archive" from matching the root "/Users/me/Dropbox":
    /// a prefix is only a parent when the next byte is a separator.
    func contains(_ path: ArraySlice<UInt8>) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        for root in roots {
            if path.count < root.count { continue }
            if path.count > root.count, path[path.startIndex + root.count] != 0x2F {
                continue
            }
            var matches = true
            for i in 0..<root.count where path[path.startIndex + i] != root[i] {
                matches = false
                break
            }
            if matches { return true }
        }
        return false
    }
}
