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

    private static func image(named name: String) -> NSImage? {
        guard
            let url = Bundle.module.url(
                forResource: name, withExtension: "png", subdirectory: "Images")
                ?? Bundle.module.url(forResource: name, withExtension: "png"),
            let image = NSImage(contentsOf: url)
        else {
            DebugLog.line("asset missing: \(name).png")
            return nil
        }
        image.isTemplate = true
        return image
    }
}
