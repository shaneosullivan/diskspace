import Foundation

/// Builds a throwaway directory tree for a test to scan.
///
/// Keys are paths relative to the root; a value is the file's contents, and a
/// key ending in "/" is an empty directory. Intermediate directories are made as
/// needed, so `["a/b/c.txt": "hi"]` produces two directories and a file.
enum TemporaryTree {
    static func make(_ entries: [String: String]) throws -> URL {
        let root = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("diskscan-tests-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)

        for (relative, contents) in entries {
            let url = root.appendingPathComponent(relative)
            if relative.hasSuffix("/") {
                try FileManager.default.createDirectory(
                    at: url, withIntermediateDirectories: true)
                continue
            }
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data(contents.utf8).write(to: url)
        }
        return root
    }

    static func remove(_ url: URL) {
        try? FileManager.default.removeItem(at: url)
    }
}
