import Foundation

/// How this copy of the app got onto the machine.
///
/// Homebrew installs the very same bundle a hand-installed copy uses — the same
/// signature, the same contents, even the same quarantine attribute — so there
/// is nothing inside the app to read. The evidence sits outside it: brew keeps a
/// symlink in its Caskroom pointing at wherever it put the app. One that points
/// back at this bundle is what makes this the copy brew manages, rather than
/// merely a copy brew installed once and someone has since replaced by hand.
///
/// It matters because the two are updated differently. Sending someone whose app
/// brew looks after to a disk image invites them to overwrite it behind brew's
/// back, leaving brew certain an older version is installed and offering to
/// "upgrade" them onto it.
enum InstallSource {
    /// The cask's token. `diskspace` belongs to something else in homebrew-cask,
    /// and a tap cannot outrank the main repository, so this app's is hyphenated.
    static let caskToken = "disk-space"

    static var upgradeCommand: String { "brew upgrade --cask \(caskToken)" }

    /// True when Homebrew owns this bundle.
    ///
    /// Read afresh each time rather than cached: a downloaded copy can be
    /// dragged over a brew-installed one between one launch and the next.
    static var isHomebrew: Bool {
        let me = Bundle.main.bundleURL.resolvingSymlinksInPath().standardizedFileURL
        let name = Bundle.main.bundleURL.lastPathComponent
        let version = UpdateCheck.currentVersion
        // A GUI app inherits no shell environment, so HOMEBREW_PREFIX cannot be
        // read here. These are the two places brew installs itself: Apple
        // silicon first, then Intel.
        var prefixes = ["/opt/homebrew", "/usr/local"]
        // Development only, so the check can be run against a fixture rather
        // than against whatever this machine happens to have installed. A
        // shipped app is launched with no environment and never sees it.
        if let extra = ProcessInfo.processInfo.environment["DISKSPACE_BREW_PREFIX"] {
            prefixes.insert(extra, at: 0)
        }
        for prefix in prefixes {
            let link = URL(
                fileURLWithPath: "\(prefix)/Caskroom/\(caskToken)/\(version)/\(name)")
            guard FileManager.default.fileExists(atPath: link.path) else { continue }
            if link.resolvingSymlinksInPath().standardizedFileURL == me { return true }
        }
        return false
    }
}
