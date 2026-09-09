import AppKit

/// Two-tab settings window: General and Treemap.
final class PreferencesWindowController: NSObject, NSToolbarDelegate {
    static let shared = PreferencesWindowController()

    private let window: NSWindow
    private let content = NSView()
    private var generalView: NSView!
    private var treemapView: NSView!
    private let resetButton = NSButton()

    private enum Tab: String { case general, treemap }
    private var tab: Tab = .general { didSet { showTab() } }

    override init() {
        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 660, height: 470),
            styleMask: [.titled, .closable], backing: .buffered, defer: false)
        super.init()
        window.title = "General Settings"
        window.center()
        window.isReleasedWhenClosed = false

        let toolbar = NSToolbar(identifier: "settings")
        toolbar.delegate = self
        toolbar.displayMode = .iconAndLabel
        toolbar.selectedItemIdentifier = .init(Tab.general.rawValue)
        window.toolbar = toolbar

        buildContent()
        window.contentView = content
        showTab()
    }

    func show() {
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    // MARK: Layout helpers

    private func checkbox(_ title: String, _ action: Selector) -> NSButton {
        let b = NSButton(checkboxWithTitle: title, target: self, action: action)
        b.translatesAutoresizingMaskIntoConstraints = false
        return b
    }

    private func explanation(_ text: String) -> NSTextField {
        let label = NSTextField(wrappingLabelWithString: text)
        label.font = .systemFont(ofSize: 11)
        label.textColor = .secondaryLabelColor
        label.translatesAutoresizingMaskIntoConstraints = false
        return label
    }

    private func caption(_ text: String) -> NSTextField {
        let label = NSTextField(labelWithString: text)
        label.alignment = .right
        label.translatesAutoresizingMaskIntoConstraints = false
        return label
    }

    /// A settings row: an optional right-aligned caption, a control, and an
    /// optional explanation beneath the control.
    private func row(caption captionText: String?, control: NSView, note: String?) -> NSView {
        let container = NSView()
        container.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(control)
        var constraints: [NSLayoutConstraint] = [
            control.topAnchor.constraint(equalTo: container.topAnchor),
            control.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 220),
            control.trailingAnchor.constraint(
                lessThanOrEqualTo: container.trailingAnchor, constant: -20),
        ]
        if let captionText {
            let label = caption(captionText)
            container.addSubview(label)
            constraints += [
                label.trailingAnchor.constraint(equalTo: control.leadingAnchor, constant: -12),
                label.firstBaselineAnchor.constraint(equalTo: control.firstBaselineAnchor),
                label.leadingAnchor.constraint(
                    greaterThanOrEqualTo: container.leadingAnchor, constant: 10),
            ]
        }
        if let note {
            let text = explanation(note)
            container.addSubview(text)
            constraints += [
                text.topAnchor.constraint(equalTo: control.bottomAnchor, constant: 3),
                text.leadingAnchor.constraint(equalTo: control.leadingAnchor, constant: 20),
                text.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -24),
                text.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            ]
        } else {
            constraints.append(control.bottomAnchor.constraint(equalTo: container.bottomAnchor))
        }
        NSLayoutConstraint.activate(constraints)
        return container
    }

    private func separator() -> NSView {
        let line = NSBox()
        line.boxType = .separator
        line.translatesAutoresizingMaskIntoConstraints = false
        return line
    }

    private func stack(_ views: [NSView]) -> NSStackView {
        let s = NSStackView(views: views)
        s.orientation = .vertical
        s.alignment = .leading
        s.spacing = 12
        s.translatesAutoresizingMaskIntoConstraints = false
        return s
    }

    // MARK: Content

    private var packageContents: NSButton!
    private var physicalSize: NSButton!
    private var splitHorizontally: NSButton!
    private var smallFiles: NSButton!
    private var smallKinds: NSButton!
    private var smallSelection: NSButton!
    private var consistentColours: NSButton!
    private var animatedZooming: NSButton!
    private var freeSpace: NSButton!
    private var otherSpace: NSButton!
    private var skipCloud: NSButton!

    private func buildContent() {
        content.translatesAutoresizingMaskIntoConstraints = false

        packageContents = checkbox("Show Package Contents", #selector(toggle(_:)))
        physicalSize = checkbox("Show Physical File Size", #selector(toggle(_:)))
        splitHorizontally = checkbox("Split window horizontally", #selector(toggle(_:)))
        smallFiles = checkbox("Files View", #selector(toggle(_:)))
        smallKinds = checkbox("Kind Statistics", #selector(toggle(_:)))
        smallSelection = checkbox("Selection List", #selector(toggle(_:)))
        consistentColours = checkbox(
            "Use the same colour for each file kind in all scans", #selector(toggle(_:)))
        animatedZooming = checkbox("Animated Zooming", #selector(toggle(_:)))
        freeSpace = checkbox("Show Free Space", #selector(toggle(_:)))
        otherSpace = checkbox("Show Other Space", #selector(toggle(_:)))
        skipCloud = checkbox("Skip cloud storage folders", #selector(toggle(_:)))

        let general = stack([
            row(caption: "View settings:", control: packageContents, note: nil),
            row(
                caption: nil, control: physicalSize,
                note:
                    "The physical size is the space a file occupies on the drive. Cloned and sparse files can hold far more content than they occupy."
            ),
            row(caption: nil, control: splitHorizontally, note: nil),
            separator(),
            row(caption: "Use small font in:", control: smallFiles, note: nil),
            row(caption: nil, control: smallKinds, note: nil),
            row(caption: nil, control: smallSelection, note: nil),
            separator(),
            row(caption: nil, control: consistentColours, note: nil),
            separator(),
            row(
                caption: "Scanning:", control: skipCloud,
                note:
                    "Folders like iCloud Drive and Dropbox hold files that may not be on this Mac. Walking one can take minutes while the service produces a listing. Skipped, they are shown but not opened — anything already downloaded into them is then left out of the totals."
            ),
        ])
        generalView = general

        let treemap = stack([
            row(
                caption: nil, control: animatedZooming,
                note: "Turn this off if the animation is slow on your machine, or you dislike it."),
            separator(),
            row(
                caption: "View settings:", control: freeSpace,
                note:
                    "Shows free space as a block in the treemap, so the scanned folder can be sized against the room left on the drive."
            ),
            row(
                caption: nil, control: otherSpace,
                note:
                    "Shows everything else in use on the drive as a block, to size the scanned folder against the rest of the drive. Unavailable when a whole drive is shown."
            ),
        ])
        treemapView = treemap

        resetButton.title = "Reset"
        resetButton.bezelStyle = .rounded
        resetButton.target = self
        resetButton.action = #selector(reset(_:))
        resetButton.translatesAutoresizingMaskIntoConstraints = false

        content.addSubview(general)
        content.addSubview(treemap)
        content.addSubview(resetButton)
        for view in [general, treemap] {
            NSLayoutConstraint.activate([
                view.topAnchor.constraint(equalTo: content.topAnchor, constant: 26),
                view.leadingAnchor.constraint(equalTo: content.leadingAnchor),
                view.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            ])
        }
        NSLayoutConstraint.activate([
            resetButton.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
            resetButton.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -18),
        ])
        syncFromPreferences()
    }

    private func showTab() {
        generalView.isHidden = tab != .general
        treemapView.isHidden = tab != .treemap
        window.title = tab == .general ? "General Settings" : "Treemap Settings"
    }

    private func syncFromPreferences() {
        let p = Preferences.shared
        packageContents.state = p.showPackageContents ? .on : .off
        physicalSize.state = p.showPhysicalFileSize ? .on : .off
        splitHorizontally.state = p.splitHorizontally ? .on : .off
        smallFiles.state = p.smallFontFilesView ? .on : .off
        smallKinds.state = p.smallFontKindStatistics ? .on : .off
        smallSelection.state = p.smallFontSelectionList ? .on : .off
        consistentColours.state = p.consistentKindColours ? .on : .off
        animatedZooming.state = p.animatedZooming ? .on : .off
        freeSpace.state = p.showFreeSpace ? .on : .off
        otherSpace.state = p.showOtherSpace ? .on : .off
        skipCloud.state = p.skipCloudFolders ? .on : .off
    }

    @objc private func toggle(_ sender: NSButton) {
        let on = sender.state == .on
        let p = Preferences.shared
        switch sender {
        case packageContents: p.showPackageContents = on
        case physicalSize: p.showPhysicalFileSize = on
        case splitHorizontally: p.splitHorizontally = on
        case smallFiles: p.smallFontFilesView = on
        case smallKinds: p.smallFontKindStatistics = on
        case smallSelection: p.smallFontSelectionList = on
        case consistentColours: p.consistentKindColours = on
        case animatedZooming: p.animatedZooming = on
        case skipCloud: p.skipCloudFolders = on
        case freeSpace: p.showFreeSpace = on
        case otherSpace: p.showOtherSpace = on
        default: break
        }
    }

    @objc private func reset(_ sender: Any?) {
        Preferences.shared.resetAll()
        syncFromPreferences()
    }

    @objc private func selectTab(_ sender: NSToolbarItem) {
        tab = Tab(rawValue: sender.itemIdentifier.rawValue) ?? .general
    }

    // MARK: NSToolbarDelegate

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.init(Tab.general.rawValue), .init(Tab.treemap.rawValue)]
    }
    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        toolbarAllowedItemIdentifiers(toolbar)
    }
    func toolbarSelectableItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        toolbarAllowedItemIdentifiers(toolbar)
    }

    func toolbar(
        _ toolbar: NSToolbar, itemForItemIdentifier identifier: NSToolbarItem.Identifier,
        willBeInsertedIntoToolbar flag: Bool
    ) -> NSToolbarItem? {
        let item = NSToolbarItem(itemIdentifier: identifier)
        let isGeneral = identifier.rawValue == Tab.general.rawValue
        item.label = isGeneral ? "General" : "Treemap"
        item.image = NSImage(
            systemSymbolName: isGeneral ? "gearshape" : "square.grid.2x2",
            accessibilityDescription: item.label)
        item.target = self
        item.action = #selector(selectTab(_:))
        return item
    }
}
