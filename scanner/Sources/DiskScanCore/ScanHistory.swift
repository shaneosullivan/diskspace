import Foundation

/// Remembers when each root was last scanned, so "added since you last looked"
/// has something to compare against.
///
/// Stored per root path in Application Support. The first scan of a root has no
/// baseline, and that bucket is reported as unavailable rather than as zero —
/// zero would read as "nothing new", which is a different claim.
public enum ScanHistory {
    private static var storeURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("DiskSpace", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base.appendingPathComponent("scan-history.json")
    }

    private static func load() -> [String: Double] {
        guard let data = try? Data(contentsOf: storeURL),
            let dict = try? JSONDecoder().decode([String: Double].self, from: data)
        else { return [:] }
        return dict
    }

    /// When `root` was last scanned, or nil if this is the first time.
    public static func lastScan(of root: String) -> Date? {
        load()[root].map { Date(timeIntervalSince1970: $0) }
    }

    public static func record(root: String, at date: Date = Date()) {
        var dict = load()
        dict[root] = date.timeIntervalSince1970
        guard let data = try? JSONEncoder().encode(dict) else { return }
        try? data.write(to: storeURL, options: .atomic)
    }

    public static func forget(root: String) {
        var dict = load()
        dict.removeValue(forKey: root)
        guard let data = try? JSONEncoder().encode(dict) else { return }
        try? data.write(to: storeURL, options: .atomic)
    }
}
