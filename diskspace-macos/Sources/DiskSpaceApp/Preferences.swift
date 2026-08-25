import AppKit

/// Application settings, persisted in user defaults.
///
/// Every change posts `didChange`, which the window observes to re-apply layout,
/// fonts, colours and the treemap without needing a rescan.
final class Preferences {
    static let shared = Preferences()
    static let didChange = Notification.Name("PreferencesDidChange")

    private let defaults = UserDefaults.standard

    private init() {
        // Defaults chosen to match what the app did before it had settings.
        defaults.register(defaults: [
            Key.showPhysicalFileSize: true,
            Key.consistentKindColours: true,
            Key.animatedZooming: true,
            Key.showFileKindStatistics: true,
        ])
    }

    private enum Key {
        static let showPackageContents = "showPackageContents"
        static let showPhysicalFileSize = "showPhysicalFileSize"
        static let splitHorizontally = "splitHorizontally"
        static let smallFontFilesView = "smallFontFilesView"
        static let smallFontKindStatistics = "smallFontKindStatistics"
        static let smallFontSelectionList = "smallFontSelectionList"
        static let consistentKindColours = "consistentKindColours"
        static let animatedZooming = "animatedZooming"
        static let showFreeSpace = "showFreeSpace"
        static let showOtherSpace = "showOtherSpace"
        static let showFileKindStatistics = "showFileKindStatistics"
        static let showSelectionList = "showSelectionList"
        static let showInformation = "showInformation"
    }

    private func flag(_ key: String) -> Bool { defaults.bool(forKey: key) }
    private func set(_ key: String, _ value: Bool) {
        defaults.set(value, forKey: key)
        NotificationCenter.default.post(name: Preferences.didChange, object: self)
    }

    /// Descend into bundles such as .app and .framework instead of drawing each as
    /// a single item.
    var showPackageContents: Bool {
        get { flag(Key.showPackageContents) }
        set { set(Key.showPackageContents, newValue) }
    }

    /// Space actually occupied on the drive, rather than the length of a file's
    /// contents. The two differ sharply where clones and sparse files are involved.
    var showPhysicalFileSize: Bool {
        get { flag(Key.showPhysicalFileSize) }
        set { set(Key.showPhysicalFileSize, newValue) }
    }

    var splitHorizontally: Bool {
        get { flag(Key.splitHorizontally) }
        set { set(Key.splitHorizontally, newValue) }
    }

    var smallFontFilesView: Bool {
        get { flag(Key.smallFontFilesView) }
        set { set(Key.smallFontFilesView, newValue) }
    }

    var smallFontKindStatistics: Bool {
        get { flag(Key.smallFontKindStatistics) }
        set { set(Key.smallFontKindStatistics, newValue) }
    }

    var smallFontSelectionList: Bool {
        get { flag(Key.smallFontSelectionList) }
        set { set(Key.smallFontSelectionList, newValue) }
    }

    /// Derive each kind's colour from the kind itself, so a given file type looks
    /// the same in every scan. Turned off, colours are assigned by size rank within
    /// the current scan, which gives the largest kinds the most distinct hues.
    var consistentKindColours: Bool {
        get { flag(Key.consistentKindColours) }
        set { set(Key.consistentKindColours, newValue) }
    }

    var animatedZooming: Bool {
        get { flag(Key.animatedZooming) }
        set { set(Key.animatedZooming, newValue) }
    }

    /// Draw the drive's free space as a block, to size the scanned folder against
    /// the room left on the drive.
    var showFreeSpace: Bool {
        get { flag(Key.showFreeSpace) }
        set { set(Key.showFreeSpace, newValue) }
    }

    /// Draw everything else in use on the drive as a block, to size the scanned
    /// folder against the rest of the drive. Meaningless when a whole drive is
    /// being shown, and hidden in that case.
    var showOtherSpace: Bool {
        get { flag(Key.showOtherSpace) }
        set { set(Key.showOtherSpace, newValue) }
    }

    var showFileKindStatistics: Bool {
        get { flag(Key.showFileKindStatistics) }
        set { set(Key.showFileKindStatistics, newValue) }
    }

    var showSelectionList: Bool {
        get { flag(Key.showSelectionList) }
        set { set(Key.showSelectionList, newValue) }
    }

    var showInformation: Bool {
        get { flag(Key.showInformation) }
        set { set(Key.showInformation, newValue) }
    }

    func resetAll() {
        for key in [
            Key.showPackageContents, Key.showPhysicalFileSize, Key.splitHorizontally,
            Key.smallFontFilesView, Key.smallFontKindStatistics, Key.smallFontSelectionList,
            Key.consistentKindColours, Key.animatedZooming, Key.showFreeSpace,
            Key.showOtherSpace,
        ] {
            defaults.removeObject(forKey: key)
        }
        NotificationCenter.default.post(name: Preferences.didChange, object: self)
    }

    /// Point size for a list, honouring the relevant "use small font" setting.
    func fontSize(small: Bool) -> CGFloat { small ? 10 : 11 }
}
