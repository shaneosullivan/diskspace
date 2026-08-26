import Foundation

/// Asks the site whether a newer version has been released.
///
/// The platform and version are sent with the request so the site can record
/// which builds are in use, and it answers with the newest release for that
/// platform along with the date the running version came out.
///
/// This is the only network request the app makes, and it carries nothing about
/// what was scanned.
enum UpdateCheck {
    struct Available {
        let version: String
        let downloads: URL
        /// When the version now running was released, as the site records it.
        /// Nil when the site has no record of it.
        let currentReleaseDate: Date?

        /// "Version 1.0.4 is available. Yours is four months old." — the second
        /// sentence only when the site knew when this build came out.
        var summary: String {
            var line = "Version \(version) is available."
            if let age = ageDescription {
                line += " Yours is \(age) old."
            }
            return line
        }

        private var ageDescription: String? {
            guard let currentReleaseDate else { return nil }
            let days = Int(Date().timeIntervalSince(currentReleaseDate) / 86400)
            guard days >= 1 else { return nil }
            if days < 45 { return days == 1 ? "a day" : "\(days) days" }
            let months = Int((Double(days) / 30.44).rounded())
            if months < 18 { return months == 1 ? "a month" : "\(months) months" }
            let years = Double(days) / 365.25
            return years < 1.5 ? "a year" : String(format: "%.0f years", years)
        }
    }

    /// What this build reports itself as. Windows and Linux builds would send
    /// their own name here, and the endpoint answers for all of them.
    static let runtime = "macos"

    /// Overridable so a development build can be pointed at a local copy of the
    /// site, the same way the refresh rate and debug logging are.
    private static var endpoint: String {
        ProcessInfo.processInfo.environment["DISKSPACE_UPDATE_URL"]
            ?? "https://diskspace.io/api/latest-version"
    }

    /// Answered once per launch: reopening About should not ask again.
    private static var cached: Available?
    private static var didCheck = false

    /// What the last check found, for drawing the panel before the next one has
    /// answered. Nil until a check has succeeded.
    static var cachedResult: Available? { cached }

    static var currentVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
            ?? "0.0.0"
    }

    /// Calls back on the main thread. Nil means "nothing newer", which is also
    /// the answer when the site cannot be reached: a failed check is not
    /// something to bother the user with.
    static func check(completion: @escaping (Available?) -> Void) {
        if didCheck {
            completion(cached)
            return
        }
        let current = currentVersion
        guard var components = URLComponents(string: endpoint) else {
            completion(nil)
            return
        }
        components.queryItems = [
            URLQueryItem(name: "platform", value: runtime),
            URLQueryItem(name: "version", value: current),
        ]
        guard let url = components.url else {
            completion(nil)
            return
        }
        var request = URLRequest(url: url)
        request.timeoutInterval = 8
        // The answer changes only when a release is cut, but a stale cache would
        // hide exactly the case this exists for.
        request.cachePolicy = .reloadIgnoringLocalCacheData

        URLSession.shared.dataTask(with: request) { data, _, _ in
            let found = data.flatMap { parse($0, current: current) }
            DispatchQueue.main.async {
                didCheck = true
                cached = found
                completion(found)
            }
        }.resume()
    }

    private static func parse(_ data: Data, current: String) -> Available? {
        guard
            let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            // Asked about one platform, the site answers about that platform.
            let mine = root["latest"] as? [String: Any],
            let version = mine["version"] as? String
        else { return nil }
        // The site does the comparison now that it runs per request; the local
        // one is the fallback if that field ever goes missing.
        let newer = root["updateAvailable"] as? Bool ?? isNewer(version, than: current)
        guard newer else { return nil }
        let downloads =
            (root["downloads"] as? String).flatMap(URL.init(string:))
            ?? URL(string: "https://diskspace.io")!
        return Available(
            version: version, downloads: downloads,
            currentReleaseDate: (root["currentDate"] as? String).flatMap(parseDate))
    }

    /// The site's dates are plain calendar days, read as UTC so the answer does
    /// not shift with the reader's time zone.
    private static func parseDate(_ text: String) -> Date? {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd"
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.locale = Locale(identifier: "en_US_POSIX")
        return formatter.date(from: text)
    }

    /// Compares dotted numeric versions a component at a time, so 1.0.10 is
    /// newer than 1.0.9 — which string comparison gets backwards.
    static func isNewer(_ candidate: String, than current: String) -> Bool {
        let a = candidate.split(separator: ".").map { Int($0) ?? 0 }
        let b = current.split(separator: ".").map { Int($0) ?? 0 }
        for i in 0..<max(a.count, b.count) {
            let x = i < a.count ? a[i] : 0
            let y = i < b.count ? b[i] : 0
            if x != y { return x > y }
        }
        return false
    }
}
