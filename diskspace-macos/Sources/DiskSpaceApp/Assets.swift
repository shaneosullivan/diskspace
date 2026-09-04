import AppKit

/// Bundled images.
///
/// Loaded as templates so AppKit tints them to match the button style and the
/// light/dark appearance, rather than baking in a single colour.
enum Assets {
    static let zoomIn = image(named: "zoom-in")
    static let zoomOut = image(named: "zoom-out")
    static let collapse = image(named: "collapse")
    /// Shown when the panel is visible: pressing it hides the panel.
    static let sideHide = image(named: "side_hide")
    /// Shown when the panel is hidden: pressing it brings the panel back.
    static let sideShow = image(named: "side_show")

    /// The app icon, for places AppKit does not fill in automatically.
    ///
    /// Read from the bundle's icon rather than shipped again as a PNG: a second
    /// full-size copy of the artwork was 568 KB, more than half the size of the
    /// program itself.
    static let appIcon: NSImage? = NSImage(named: NSImage.applicationIconName)

    /// A copy of `image` drawn at `points` square.
    ///
    /// The source art is 128px; left at that size a button scales it to fill its
    /// whole height, leaving no breathing room around the glyph.
    static func sized(_ image: NSImage?, _ points: CGFloat) -> NSImage? {
        guard let image, let copy = image.copy() as? NSImage else { return nil }
        copy.size = NSSize(width: points, height: points)
        copy.isTemplate = image.isTemplate
        return copy
    }

    /// The bundle the images live in.
    ///
    /// Found here rather than through SwiftPM's `Bundle.module`, which looks in
    /// exactly two places: beside the executable, and an absolute path inside
    /// whichever machine compiled the binary. Wrapped in a .app the resources
    /// sit in Contents/Resources, so neither matched — and on the build machine
    /// the second path existed, so it worked there and only there. Worse, that
    /// accessor calls fatalError when it finds nothing, so every other machine
    /// crashed on launch instead of quietly losing an icon.
    ///
    /// Nil is a normal answer. An app missing a toolbar glyph is a blemish; an
    /// app that will not open is not.
    private static let resources: Bundle? = {
        let name = "diskscan_DiskSpaceApp.bundle"
        let candidates = [
            // Packaged: Contents/Resources.
            Bundle.main.resourceURL,
            // Run straight out of .build: beside the executable.
            Bundle.main.executableURL?.deletingLastPathComponent(),
            Bundle.main.bundleURL,
        ]
        for base in candidates {
            guard let url = base?.appendingPathComponent(name) else { continue }
            if let bundle = Bundle(url: url) {
                return bundle
            }
        }
        DebugLog.line("resource bundle not found: \(name)")
        return nil
    }()

    private static func image(named name: String) -> NSImage? {
        guard
            let resources,
            let url = resources.url(
                forResource: name, withExtension: "png", subdirectory: "Images")
                ?? resources.url(forResource: name, withExtension: "png"),
            let image = NSImage(contentsOf: url)
        else {
            DebugLog.line("asset missing: \(name).png")
            return nil
        }
        image.isTemplate = true
        return image
    }
}
