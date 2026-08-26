import AppKit
import DiskScanCore

// MARK: - Kind statistics column

final class KindTableController: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    private weak var tableView: NSTableView?
    private let palette: KindPalette
    private var rows: [(kind: Int16, bytes: Int64, count: Int)] = []
    private var sortKey = "size"
    private var ascending = false
    var session: ScanSession?
    /// Set from settings; rows redraw at the smaller size on the next refresh.
    var smallFont = false

    init(tableView: NSTableView, palette: KindPalette) {
        self.tableView = tableView
        self.palette = palette
        super.init()
        tableView.dataSource = self
        tableView.delegate = self
    }

    func refresh() {
        rows =
            session?.scanner?.kindTotals(
                metric: session?.metric ?? .total,
                useLogicalSize: !Preferences.shared.showPhysicalFileSize) ?? []
        applySort()
        tableView?.reloadData()
    }

    private func applySort() {
        switch sortKey {
        case "kind":
            rows.sort { FileKind.names[Int($0.kind)] < FileKind.names[Int($1.kind)] }
        case "files":
            rows.sort { $0.count < $1.count }
        default:
            rows.sort { $0.bytes < $1.bytes }
        }
        if !ascending { rows.reverse() }
    }

    func numberOfRows(in tableView: NSTableView) -> Int { rows.count }

    func tableView(
        _ tableView: NSTableView, sortDescriptorsDidChange oldDescriptors: [NSSortDescriptor]
    ) {
        guard let d = tableView.sortDescriptors.first, let key = d.key else { return }
        sortKey = key
        ascending = d.ascending
        applySort()
        tableView.reloadData()
    }

    func tableView(
        _ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int
    ) -> NSView? {
        guard row < rows.count, let id = tableColumn?.identifier.rawValue else { return nil }
        let entry = rows[row]
        let cell = NSTableCellView()
        let text = NSTextField(labelWithString: "")
        text.font = .systemFont(ofSize: Preferences.shared.fontSize(small: smallFont))
        text.lineBreakMode = .byTruncatingTail
        text.translatesAutoresizingMaskIntoConstraints = false

        switch id {
        case "colour":
            let swatch = NSView()
            swatch.wantsLayer = true
            swatch.layer?.backgroundColor = palette.nsColor(forKind: entry.kind).cgColor
            swatch.layer?.cornerRadius = 2
            swatch.translatesAutoresizingMaskIntoConstraints = false
            cell.addSubview(swatch)
            NSLayoutConstraint.activate([
                swatch.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 2),
                swatch.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
                swatch.widthAnchor.constraint(equalToConstant: 12),
                swatch.heightAnchor.constraint(equalToConstant: 12),
            ])
            return cell
        case "kind":
            text.stringValue = FileKind.names[Int(entry.kind)]
        case "size":
            text.stringValue = Format.bytes(entry.bytes)
            text.alignment = .right
        default:
            text.stringValue = Format.count(entry.count)
            text.alignment = .right
        }

        cell.addSubview(text)
        cell.textField = text
        NSLayoutConstraint.activate([
            text.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 4),
            text.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -4),
            text.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
        ])
        return cell
    }
}

// MARK: - File tree column

final class TreeController: NSObject, NSOutlineViewDataSource, NSOutlineViewDelegate {
    private weak var outlineView: NSOutlineView?
    private let palette: KindPalette
    var session: ScanSession?
    var onSelect: ((Int32) -> Void)?
    /// Fired whenever the visible row set changes, so a pinned column can follow.
    var onRowsChanged: (() -> Void)?

    /// Children sorted by size, cached per node and invalidated by generation so a
    /// live scan does not resort the whole tree on every refresh.
    private var sortedCache: [Int32: [Int32]] = [:]
    private var cacheGeneration = -1
    private var cacheMetric: SizeMetric = .total
    private var didExpandRoot = false
    /// Set from settings; rows redraw at the smaller size on the next refresh.
    var smallFont = false

    init(outlineView: NSOutlineView, palette: KindPalette) {
        self.outlineView = outlineView
        self.palette = palette
        super.init()
        outlineView.dataSource = self
        outlineView.delegate = self
    }

    func refresh() {
        guard let outlineView, let session else { return }
        if cacheGeneration != session.generation || cacheMetric != session.metric {
            sortedCache.removeAll(keepingCapacity: true)
            cacheGeneration = session.generation
            cacheMetric = session.metric
        }
        // Preserves expansion because items compare equal by node index.
        outlineView.reloadData()

        // Open the root as soon as it exists, so the first thing the user sees is
        // the top-level breakdown rather than a single collapsed row.
        if !didExpandRoot, let arena = session.arena, arena.count > 0 {
            outlineView.expandItem(NSNumber(value: Int32(0)))
            didExpandRoot = true
        }
        onRowsChanged?()
    }

    /// Called when a new scan begins, so the next root expands again.
    func resetExpansion() {
        didExpandRoot = false
        sortedCache.removeAll(keepingCapacity: true)
    }

    /// Collapses everything back to the root row. The next refresh re-expands the
    /// root, so the user lands on the top-level breakdown for the new figure
    /// rather than on branches chosen for the previous one.
    func collapseToRoot() {
        outlineView?.collapseItem(nil, collapseChildren: true)
        didExpandRoot = false
    }

    private func children(of node: Int32) -> [Int32] {
        if let cached = sortedCache[node] { return cached }
        guard let arena = session?.arena, Int(node) < arena.count else { return [] }
        let range = arena.children(of: Int(node))
        let metric = session?.metric ?? .total
        var kids = range.map { Int32($0) }
        kids.sort { arena.size(metric, Int($0)) > arena.size(metric, Int($1)) }
        sortedCache[node] = kids
        return kids
    }

    private func node(from item: Any?) -> Int32 {
        (item as? NSNumber)?.int32Value ?? 0
    }

    func outlineView(_ outlineView: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int {
        guard let arena = session?.arena, arena.count > 0 else { return 0 }
        if item == nil { return 1 }  // the root row
        return children(of: node(from: item)).count
    }

    func outlineView(_ outlineView: NSOutlineView, child index: Int, ofItem item: Any?) -> Any {
        if item == nil { return NSNumber(value: Int32(0)) }
        let kids = children(of: node(from: item))
        return NSNumber(value: index < kids.count ? kids[index] : 0)
    }

    func outlineView(_ outlineView: NSOutlineView, isItemExpandable item: Any) -> Bool {
        guard let arena = session?.arena else { return false }
        let n = Int(node(from: item))
        guard n < arena.count else { return false }
        return arena.isDirectory(n) && !arena.children(of: n).isEmpty
    }

    func outlineView(
        _ outlineView: NSOutlineView, viewFor tableColumn: NSTableColumn?, item: Any
    ) -> NSView? {
        guard let arena = session?.arena, let id = tableColumn?.identifier.rawValue else {
            return nil
        }
        let n = Int(node(from: item))
        guard n < arena.count else { return nil }

        let cell = NSTableCellView()
        let text = NSTextField(labelWithString: "")
        text.font = .systemFont(ofSize: Preferences.shared.fontSize(small: smallFont))
        text.lineBreakMode = .byTruncatingTail
        text.translatesAutoresizingMaskIntoConstraints = false

        if id == "name" {
            let swatch = NSView()
            swatch.wantsLayer = true
            swatch.layer?.backgroundColor = palette.nsColor(forKind: arena.kind[n]).cgColor
            swatch.layer?.cornerRadius = 2
            swatch.translatesAutoresizingMaskIntoConstraints = false
            cell.addSubview(swatch)
            cell.addSubview(text)
            text.stringValue = n == 0 ? (session?.rootPath ?? "/") : arena.name(n)
            NSLayoutConstraint.activate([
                swatch.leadingAnchor.constraint(equalTo: cell.leadingAnchor),
                swatch.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
                swatch.widthAnchor.constraint(equalToConstant: 10),
                swatch.heightAnchor.constraint(equalToConstant: 10),
                text.leadingAnchor.constraint(equalTo: swatch.trailingAnchor, constant: 5),
                text.trailingAnchor.constraint(lessThanOrEqualTo: cell.trailingAnchor, constant: -4),
                text.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
            ])
        }
        cell.textField = text
        return cell
    }

    func outlineViewSelectionDidChange(_ notification: Notification) {
        guard let outlineView, outlineView.selectedRow >= 0,
            let item = outlineView.item(atRow: outlineView.selectedRow)
        else { return }
        onSelect?(node(from: item))
        onRowsChanged?()
    }

    /// Expands the path to `node` and selects it, so treemap clicks drive the tree.
    func reveal(_ target: Int32) {
        guard let outlineView, let arena = session?.arena else { return }
        var chain: [Int32] = []
        var cur = Int(target)
        while cur > 0 {
            chain.append(Int32(cur))
            cur = Int(arena.parent[cur])
        }
        chain.append(0)
        for n in chain.reversed().dropLast() {
            outlineView.expandItem(NSNumber(value: n))
        }
        let row = outlineView.row(forItem: NSNumber(value: target))
        guard row >= 0 else { return }
        outlineView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
        centreRow(row)
    }

    /// Scrolls so `row` sits as close to the middle of the list as the content
    /// allows.
    ///
    /// `scrollRowToVisible` moves the minimum distance, which leaves a row clicked
    /// from the map pinned to the very bottom edge — visible, but with none of the
    /// surrounding folders that give it context.
    private func centreRow(_ row: Int) {
        guard let outlineView, let clip = outlineView.enclosingScrollView?.contentView else {
            return
        }
        let rowRect = outlineView.rect(ofRow: row)
        guard rowRect.height > 0 else { return }

        let viewport = clip.bounds.height
        var y = rowRect.midY - viewport / 2
        let maxY = max(0, outlineView.bounds.height - viewport)
        y = max(0, min(y, maxY))

        clip.scroll(to: CGPoint(x: clip.bounds.origin.x, y: y))
        outlineView.enclosingScrollView?.reflectScrolledClipView(clip)
        onRowsChanged?()
    }

    func selectedNode() -> Int32? {
        guard let outlineView, outlineView.selectedRow >= 0,
            let item = outlineView.item(atRow: outlineView.selectedRow)
        else { return nil }
        return node(from: item)
    }
}

// MARK: - Main window

final class MainWindowController: NSObject, TreemapViewDelegate, NSMenuItemValidation,
    AppCommands, NSMenuDelegate
{
    private let window: NSWindow
    private let session = ScanSession()
    private let palette = KindPalette()

    private let kindTable = NSTableView()
    private let treePane = TreePaneView()
    private var kindPane: NSView!
    private var outlineView: NSOutlineView { treePane.outlineView }
    private let treemapView = TreemapView()
    private let statusLabel = NSTextField(labelWithString: "")
    private let zoomOutButton = NSButton()
    private let stopButton = NSButton()
    private let refreshButton = NSButton()
    private let openButton = NSButton()
    private let metricPopUp = NSPopUpButton()

    private var kindController: KindTableController!
    private var treeController: TreeController!
    private var welcomeView: WelcomeView!
    /// AppKit's about panel, kept so a late update check can redraw it — but
    /// only while it is still on screen.
    private weak var aboutPanel: NSWindow?
    private var splitView: NSSplitView!
    private let tooltipView = TooltipView()
    /// Scan generation whose treemap has already been dumped for diagnostics.
    private var dumpedGeneration = -1
    /// Tracks the panel's visibility so reopening can be told from other setting
    /// changes, which must not disturb a width the user has chosen.
    private var kindPaneWasVisible = true

    /// Initial width of the folder pane. The map is what the eye reads first, so
    /// the list starts narrow enough to leave the map the larger share; the
    /// divider is the user's from then on.
    private static let treePaneInitialWidth: CGFloat = 360

    /// Height of the toolbar strip above the panes.
    private static let toolbarHeight: CGFloat = 42
    /// The window opens at this and stays there. Resizing on the first scan
    /// meant the app moved under the user at the moment they were reading it.
    private static let defaultSize = NSSize(width: 1400, height: 900)

    override init() {
        window = NSWindow(
            contentRect: NSRect(
                x: 0, y: 0, width: Self.defaultSize.width, height: Self.defaultSize.height),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false)
        super.init()
        window.title = "Disk Space"
        window.center()
        buildUI()
        session.onRefresh = { [weak self] in self?.refresh(live: true) }
        MainMenu.install(target: self)
        NotificationCenter.default.addObserver(
            self, selector: #selector(preferencesChanged),
            name: Preferences.didChange, object: nil)
        DebugLog.announce()
        treemapView.onRendered = { [weak self] image, options in
            guard let self, !self.session.isScanning, let arena = self.session.arena else {
                return
            }
            // Once only per completed scan: the dump is large.
            guard self.dumpedGeneration != self.session.generation else { return }
            self.dumpedGeneration = self.session.generation
            DebugLog.dumpTreemap(
                image, arena: arena, palette: self.palette, options: options,
                rootPath: self.session.rootPath)
        }
        SelectionListPanelController.shared.onChoose = { [weak self] node in
            self?.treeController.reveal(node)
            self?.treemapView.highlightedNode = node
        }
        applyPreferences()
    }

    func show() {
        window.makeKeyAndOrderFront(nil)
        // Asked once at launch. The answer is cached, so opening About later
        // costs nothing and shows the same thing.
        UpdateCheck.check { [weak self] available in
            guard let self, let available, self.welcomeView != nil else { return }
            self.welcomeView.showUpdate(available)
        }
        NotificationCenter.default.addObserver(
            self, selector: #selector(appBecameActive),
            name: NSApplication.didBecomeActiveNotification, object: nil)
    }

    @objc private func appBecameActive() {
        guard welcomeView != nil, !welcomeView.isHidden else { return }
        welcomeView.refreshAccessState()
    }

    func scan(path: String) {
        InformationPanelController.shared.clear()
        SelectionListPanelController.shared.clear()
        window.title = "Disk Space — \(path)"
        welcomeView.isHidden = true
        splitView.isHidden = false
        treemapView.setArena(nil)
        treemapView.highlightedNode = nil
        treemapView.resetLivePacing()
        treeController.resetExpansion()
        stopButton.isEnabled = true
        stopButton.title = "Stop"
        session.start(path: path)
        treemapView.setArena(session.arena)
        refresh()
    }

    // MARK: UI construction

    private func buildUI() {
        let split = NSSplitView()
        split.isVertical = true
        split.dividerStyle = .thin
        split.translatesAutoresizingMaskIntoConstraints = false
        split.addArrangedSubview(makeKindPane())
        split.addArrangedSubview(makeTreePane())
        split.addArrangedSubview(makeTreemapPane())
        splitView = split
        // The divider can be dragged shut without the setting changing, so the
        // button follows the pane's real width.
        NotificationCenter.default.addObserver(
            self, selector: #selector(splitViewResized(_:)),
            name: NSSplitView.didResizeSubviewsNotification, object: split)

        welcomeView = WelcomeView(
            onPick: { [weak self] path in self?.scan(path: path) },
            onBrowse: { [weak self] in self?.chooseTarget() })

        let toolbar = makeToolbar()
        let content = NSView()
        content.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(toolbar)
        content.addSubview(split)
        content.addSubview(welcomeView)
        // Last, so it sits above the panes and is never clipped by one.
        content.addSubview(tooltipView)
        NSLayoutConstraint.activate([
            toolbar.topAnchor.constraint(equalTo: content.topAnchor),
            toolbar.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            toolbar.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            toolbar.heightAnchor.constraint(equalToConstant: Self.toolbarHeight),
            split.topAnchor.constraint(equalTo: toolbar.bottomAnchor),
            split.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            split.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            split.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            welcomeView.topAnchor.constraint(equalTo: toolbar.bottomAnchor),
            welcomeView.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            welcomeView.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            welcomeView.bottomAnchor.constraint(equalTo: content.bottomAnchor),
        ])
        // Nothing scanned yet: the three panes would be empty, so offer choices instead.
        split.isHidden = true
        window.contentView = content
        installTitlebarIcon()

        DispatchQueue.main.async { [weak self] in self?.layoutPanes() }
    }

    /// Places both dividers: the file-kind pane at exactly the width its columns
    /// need, and the folder pane at its starting width, leaving the rest to the
    /// map.
    ///
    /// This has to run again after the window grows for scanning. A split view
    /// shares new width out among all three panes, which leaves the kind pane
    /// wider than it was but its columns wider still, so the last one clips.
    private func layoutPanes() {
        let kindWidth = kindPaneRequiredWidth
        splitView.setPosition(kindWidth, ofDividerAt: 0)
        splitView.setPosition(kindWidth + Self.treePaneInitialWidth, ofDividerAt: 1)
    }

    /// Puts the app icon at the leading edge of the title bar.
    ///
    /// macOS shows an icon there only for document windows, via the proxy icon.
    /// This app has no document, so the icon goes in a titlebar accessory instead.
    private func installTitlebarIcon() {
        guard let icon = Assets.appIcon else { return }
        let imageView = NSImageView(image: icon)
        imageView.imageScaling = .scaleProportionallyDown
        imageView.translatesAutoresizingMaskIntoConstraints = false

        let holder = NSView()
        holder.addSubview(imageView)
        NSLayoutConstraint.activate([
            imageView.widthAnchor.constraint(equalToConstant: 18),
            imageView.heightAnchor.constraint(equalToConstant: 18),
            imageView.leadingAnchor.constraint(equalTo: holder.leadingAnchor, constant: 6),
            imageView.trailingAnchor.constraint(equalTo: holder.trailingAnchor, constant: -2),
            imageView.centerYAnchor.constraint(equalTo: holder.centerYAnchor),
            holder.heightAnchor.constraint(equalToConstant: 22),
        ])

        let accessory = NSTitlebarAccessoryViewController()
        accessory.view = holder
        accessory.layoutAttribute = .leading
        window.addTitlebarAccessoryViewController(accessory)
    }

    private func makeToolbar() -> NSView {
        let bar = NSView()
        bar.translatesAutoresizingMaskIntoConstraints = false

        openButton.title = "Open…"
        openButton.target = self
        openButton.action = #selector(showWelcome)
        openButton.bezelStyle = .rounded
        openButton.translatesAutoresizingMaskIntoConstraints = false
        // Both of these act on a scan, so they have nothing to offer the chooser
        // — which starts on screen, hence hidden from the outset.
        openButton.isHidden = true
        metricPopUp.isHidden = true

        metricPopUp.translatesAutoresizingMaskIntoConstraints = false
        metricPopUp.target = self
        metricPopUp.action = #selector(metricChanged)
        for m in SizeMetric.allCases { metricPopUp.addItem(withTitle: m.title) }
        metricPopUp.selectItem(at: SizeMetric.total.rawValue)

        // Zooming in happens by double-clicking a block or from the View menu, so
        // only the way back out needs a button.
        //
        // The glyph is drawn at a fixed 14pt with a space before the label, and
        // the button is given a width beyond what its content needs, so the icon
        // and text sit in the middle with even margins rather than filling the
        // bezel edge to edge.
        zoomOutButton.image = Assets.sized(Assets.zoomOut, 14)
        zoomOutButton.imagePosition = .imageLeading
        zoomOutButton.imageScaling = .scaleNone
        zoomOutButton.attributedTitle = NSAttributedString(
            string: "  Zoom Out",
            attributes: [.font: NSFont.systemFont(ofSize: 13, weight: .regular)])
        zoomOutButton.bezelStyle = .texturedRounded
        zoomOutButton.controlSize = .large
        zoomOutButton.target = self
        zoomOutButton.action = #selector(zoomOutButtonPressed)
        zoomOutButton.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            zoomOutButton.heightAnchor.constraint(equalToConstant: 32),
            zoomOutButton.widthAnchor.constraint(greaterThanOrEqualToConstant: 124),
        ])
        zoomOutButton.isHidden = true
        zoomOutButton.translatesAutoresizingMaskIntoConstraints = false

        stopButton.title = "Stop"
        stopButton.bezelStyle = .rounded
        stopButton.target = self
        stopButton.action = #selector(stopScan)
        stopButton.toolTip = "Stop scanning and keep what has been found so far"
        stopButton.isHidden = true
        stopButton.translatesAutoresizingMaskIntoConstraints = false

        refreshButton.title = "Refresh"
        refreshButton.bezelStyle = .rounded
        refreshButton.target = self
        refreshButton.action = #selector(refreshEverything)
        refreshButton.toolTip = "Scan the whole tree again"
        refreshButton.isHidden = true
        refreshButton.translatesAutoresizingMaskIntoConstraints = false

        statusLabel.font = .systemFont(ofSize: 11)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.translatesAutoresizingMaskIntoConstraints = false

        bar.addSubview(openButton)
        bar.addSubview(metricPopUp)
        bar.addSubview(statusLabel)
        bar.addSubview(stopButton)
        bar.addSubview(zoomOutButton)
        bar.addSubview(refreshButton)
        NSLayoutConstraint.activate([
            openButton.leadingAnchor.constraint(equalTo: bar.leadingAnchor, constant: 10),
            openButton.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            metricPopUp.leadingAnchor.constraint(equalTo: openButton.trailingAnchor, constant: 10),
            metricPopUp.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            metricPopUp.widthAnchor.constraint(equalToConstant: 210),
            statusLabel.leadingAnchor.constraint(equalTo: metricPopUp.trailingAnchor, constant: 14),
            statusLabel.centerYAnchor.constraint(equalTo: bar.centerYAnchor),

            refreshButton.trailingAnchor.constraint(equalTo: bar.trailingAnchor, constant: -10),
            refreshButton.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            stopButton.trailingAnchor.constraint(
                equalTo: refreshButton.leadingAnchor, constant: -8),
            stopButton.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            zoomOutButton.trailingAnchor.constraint(
                equalTo: stopButton.leadingAnchor, constant: -8),
            zoomOutButton.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            statusLabel.trailingAnchor.constraint(
                lessThanOrEqualTo: zoomOutButton.leadingAnchor, constant: -10),
        ])
        return bar
    }

    private func makeKindPane() -> NSView {
        kindTable.usesAlternatingRowBackgroundColors = true
        kindTable.rowHeight = 18
        kindTable.allowsColumnResizing = true

        let colour = NSTableColumn(identifier: .init("colour"))
        colour.title = ""
        colour.width = 18
        colour.minWidth = 18
        colour.maxWidth = 18
        kindTable.addTableColumn(colour)

        let kind = NSTableColumn(identifier: .init("kind"))
        kind.title = "Kind"
        kind.width = 130
        kind.minWidth = 130
        kind.sortDescriptorPrototype = NSSortDescriptor(key: "kind", ascending: true)
        kindTable.addTableColumn(kind)

        let size = NSTableColumn(identifier: .init("size"))
        size.title = "Size"
        size.width = 72
        size.minWidth = 72
        size.sortDescriptorPrototype = NSSortDescriptor(key: "size", ascending: false)
        kindTable.addTableColumn(size)

        let files = NSTableColumn(identifier: .init("files"))
        files.title = "Files"
        files.width = 66
        files.minWidth = 66
        files.sortDescriptorPrototype = NSSortDescriptor(key: "files", ascending: false)
        kindTable.addTableColumn(files)

        kindTable.sortDescriptors = [NSSortDescriptor(key: "size", ascending: false)]
        kindController = KindTableController(tableView: kindTable, palette: palette)
        kindController.session = session
        let pane = scrollWrapping(kindTable)
        kindPane = pane
        return pane
    }

    private func makeTreePane() -> NSView {
        let name = NSTableColumn(identifier: .init("name"))
        let header = InsetHeaderCell(textCell: "Name")
        header.alignment = .left
        header.font = NSFont.systemFont(ofSize: 11)
        // Both header buttons share this row: they start at 5pt and occupy
        // 16 + 6 + 16pt, ending at 43pt. The title sits 8pt past that.
        header.leadingInset = 51
        name.headerCell = header
        name.title = "Name"
        name.width = 320
        name.minWidth = 120
        outlineView.addTableColumn(name)

        treePane.collapseButton.target = self
        treePane.collapseButton.action = #selector(collapseAll)
        treePane.sidePanelButton.target = self
        treePane.sidePanelButton.action = #selector(toggleSidePanel)
        outlineView.outlineTableColumn = name

        treePane.sizeTextForRow = { [weak self] row in
            guard let self, let arena = self.session.arena,
                let item = self.outlineView.item(atRow: row) as? NSNumber
            else { return "" }
            let node = Int(item.int32Value)
            guard node < arena.count else { return "" }
            return Format.bytes(self.displaySize(arena, node))
        }
        treePane.nameTextForRow = { [weak self] row in
            guard let self, let arena = self.session.arena,
                let item = self.outlineView.item(atRow: row) as? NSNumber
            else { return "" }
            let node = Int(item.int32Value)
            guard node < arena.count else { return "" }
            return node == 0 ? self.session.rootPath : arena.name(node)
        }

        treeController = TreeController(outlineView: outlineView, palette: palette)
        treeController.onRowsChanged = { [weak self] in self?.treePane.reloadPinnedColumn() }
        treeController.session = session
        treeController.onSelect = { [weak self] node in
            guard let self else { return }
            // Walking down the list walks the highlight into smaller parts of the
            // same rectangle, which is what keeps the map usable as a fixed frame
            // of reference.
            self.treemapView.highlightedNode = node
            self.updateInspectors(for: node)
        }

        let menu = NSMenu()
        menu.addItem(
            NSMenuItem(title: "Reveal in Finder", action: #selector(revealInFinder), keyEquivalent: ""))
        menu.addItem(
            NSMenuItem(title: "Copy Path", action: #selector(copyPath), keyEquivalent: ""))
        menu.addItem(
            NSMenuItem(title: "Zoom Treemap Here", action: #selector(zoomTreemapHere), keyEquivalent: ""))
        menu.addItem(
            NSMenuItem(title: "Refresh", action: #selector(refreshFolder), keyEquivalent: ""))
        menu.addItem(.separator())
        let trash = NSMenuItem(
            title: "Move to Trash…", action: #selector(moveToTrash), keyEquivalent: "")
        menu.addItem(trash)
        for item in menu.items { item.target = self }
        outlineView.menu = menu

        return treePane
    }

    private func makeTreemapPane() -> NSView {
        treemapView.delegate = self
        treemapView.translatesAutoresizingMaskIntoConstraints = false
        return treemapView
    }

    private func scrollWrapping(_ view: NSView) -> NSScrollView {
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.hasHorizontalScroller = true
        scroll.autohidesScrollers = true
        scroll.documentView = view
        scroll.translatesAutoresizingMaskIntoConstraints = false
        return scroll
    }

    // MARK: Refresh

    /// `live` marks the periodic tick during a scan, which lets the treemap pace
    /// itself. Everything else — settings, metric changes, scan completion — must
    /// redraw at once.
    private func refresh(live: Bool = false) {
        updateMetricAvailability()
        if !palette.useConsistentColours, let scanner = session.scanner {
            palette.setRanking(
                scanner.kindTotals(metric: session.metric)
                    .sorted { $0.bytes > $1.bytes }.map(\.kind))
        }
        kindController.refresh()
        treeController.refresh()
        if live { treemapView.liveRefresh() } else { treemapView.refresh() }
        zoomOutButton.isHidden = !treemapView.canZoomOut
        stopButton.isHidden = !session.isScanning
        // These belong to the results, so they appear with them and never on the
        // chooser.
        let onChooser = splitView.isHidden
        refreshButton.isHidden = onChooser
        openButton.isHidden = onChooser
        metricPopUp.isHidden = onChooser
        refreshButton.isEnabled = !session.isScanning

        guard let arena = session.arena, arena.count > 0 else {
            statusLabel.stringValue = session.isScanning ? "Starting scan…" : ""
            return
        }
        let nodes = arena.count
        let files = Int(arena.fileCount[0])
        let bytes = arena.allocated[0]
        let counts =
            "\(Format.count(files)) files · \(Format.count(nodes - files)) folders · \(Format.bytes(bytes))"
        if session.isScanning {
            statusLabel.stringValue =
                "Scanning… \(counts) · \(String(format: "%.1fs", session.elapsed))"
        } else {
            let headline = session.wasStopped ? "Scan stopped" : "Scan complete"
            var line = "\(headline) · \(counts) · \(String(format: "%.1fs", session.elapsed))"
            if session.metric != .total {
                line += "  —  \(session.metric.title): \(Format.bytes(arena.size(session.metric, 0)))"
            }
            statusLabel.stringValue = line
        }
    }

    // MARK: Actions

    /// Returns to the chooser rather than straight to the file panel, so the
    /// quick picks are on offer again and not just "some other folder". The
    /// window keeps whatever size it has: it was sized for a treemap, and the
    /// chooser centres itself in however much room it is given.
    @objc func showWelcome() {
        guard welcomeView.isHidden else { return }
        // Nothing leads back to the results from here, so a scan still running
        // would be publishing into views no one can reach.
        session.cancel()
        treemapView.setArena(nil)
        treemapView.highlightedNode = nil
        InformationPanelController.shared.clear()
        SelectionListPanelController.shared.clear()
        window.title = "Disk Space"
        splitView.isHidden = true
        welcomeView.isHidden = false
        welcomeView.refreshAccessState()
        refresh()
    }

    /// The standard about panel, filled in properly.
    ///
    /// AppKit's default shows the name, version and copyright and nothing else.
    /// The credits field takes an attributed string, and the panel renders it in
    /// a text view, so links in it are live.
    @objc func showAbout(_ sender: Any?) {
        presentAbout(update: UpdateCheck.cachedResult)
        // Asked on every open rather than at launch, so it is the user's own
        // action that reaches the network, and never a background one.
        UpdateCheck.check { [weak self] available in
            guard let self, let available else { return }
            // Only redraw a panel the user still has open.
            guard self.aboutPanel?.isVisible == true else { return }
            self.presentAbout(update: available)
        }
    }

    /// The standard about panel, filled in properly.
    ///
    /// AppKit's default shows the name, version and copyright and nothing else.
    /// The credits field takes an attributed string, and the panel renders it in
    /// a text view, so links in it are live.
    private func presentAbout(update: UpdateCheck.Available?) {
        let body = NSMutableAttributedString()
        let base: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 11),
            .foregroundColor: NSColor.labelColor,
        ]
        let centred = NSMutableParagraphStyle()
        centred.alignment = .center
        centred.lineSpacing = 2
        var paragraph = base
        paragraph[.paragraphStyle] = centred

        body.append(
            NSAttributedString(
                string: "Find out where your disk space went. Scans a folder or a "
                    + "whole volume in parallel and shows the results as they arrive, "
                    + "with a treemap of every file and four measures of size — "
                    + "including what has only appeared recently.\n\n",
                attributes: paragraph))

        if let update {
            var notice = paragraph
            notice[.font] = NSFont.systemFont(ofSize: 11, weight: .semibold)
            body.append(
                NSAttributedString(
                    string: "Version \(update.version) is available. ",
                    attributes: notice))
            body.append(
                link("Download it", update.downloads.absoluteString, base: notice))
            body.append(NSAttributedString(string: "\n\n", attributes: paragraph))
        }

        body.append(NSAttributedString(string: "Built by ", attributes: paragraph))
        body.append(link("Chofter Apps", "https://www.chofter.com", base: paragraph))
        body.append(NSAttributedString(string: "\n", attributes: paragraph))
        body.append(link("diskspace.io", "https://diskspace.io", base: paragraph))

        NSApp.orderFrontStandardAboutPanel(options: [
            .applicationName: "Disk Space",
            .credits: body,
        ])
        NSApp.activate(ignoringOtherApps: true)
        aboutPanel = NSApp.keyWindow
    }

    private func link(
        _ text: String, _ url: String, base: [NSAttributedString.Key: Any]
    ) -> NSAttributedString {
        var attributes = base
        attributes[.link] = URL(string: url)!
        return NSAttributedString(string: text, attributes: attributes)
    }

    /// Scans the right-clicked folder again, and only that folder.
    ///
    /// Everything recorded under it is thrown away first, so a folder whose
    /// contents have been deleted since the scan drops to zero instead of
    /// keeping figures that no longer describe anything.
    @objc func refreshFolder() {
        guard !session.isScanning, let node = treeController.selectedNode(),
            let arena = session.arena, arena.isDirectory(Int(node))
        else { return }
        treemapView.highlightedNode = nil
        treeController.resetExpansion()
        session.refresh(node: node)
        refresh()
    }

    /// Scans the whole tree again from the top.
    @objc func refreshEverything() {
        guard !session.isScanning, !session.rootPath.isEmpty else { return }
        scan(path: session.rootPath)
    }

    /// Ends the walk where it stands. What has been found stays on screen and
    /// stays usable — a partial answer is often the answer.
    @objc func stopScan() {
        session.stop()
        stopButton.isEnabled = false
        stopButton.title = "Stopping…"
    }

    @objc func chooseTarget() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.message = "Choose a folder or volume to scan"
        // A disk-space tool has to be able to reach the folders that actually fill
        // up. ~/Library carries the hidden flag, and dot-directories like .gradle
        // and .npm are some of the largest things in a home folder — the panel
        // hides all of them by default.
        panel.showsHiddenFiles = true
        // Bundles are directories worth measuring, not opaque single items.
        panel.treatsFilePackagesAsDirectories = true
        panel.directoryURL = URL(fileURLWithPath: NSHomeDirectory())
        panel.begin { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.scan(path: url.path)
        }
    }

    @objc private func metricChanged() {
        guard let m = SizeMetric(rawValue: metricPopUp.indexOfSelectedItem) else { return }
        session.metric = m
        treemapView.metric = m
        // Both views start over: a drill-down that made sense for total size is
        // rarely where the recently-added bytes are.
        treemapView.resetZoom()
        treemapView.highlightedNode = nil
        treeController.collapseToRoot()
        // The size column header names whichever figure is on show, so a screenshot
        // of the window is never ambiguous about what it is measuring.
        outlineView.tableColumns.first(where: { $0.identifier.rawValue == "size" })?.title =
            m == .total ? "Size" : m.title
        refresh()
    }

    /// "Added Since Last Scan" is meaningless without a baseline; say so rather
    /// than silently showing zeros.
    private func updateMetricAvailability() {
        let hasBaseline = session.sinceLastScanBaseline != nil
        if let item = metricPopUp.item(at: SizeMetric.addedSinceLastScan.rawValue) {
            item.isEnabled = hasBaseline
            item.title = hasBaseline
                ? SizeMetric.addedSinceLastScan.title
                : SizeMetric.addedSinceLastScan.title + " (no previous scan)"
        }
    }

    @objc private func zoomOutButtonPressed() { treemapView.zoomOut() }

    /// Whether the file-kind panel is actually on screen. Not the same as the
    /// setting: the divider can be dragged shut, which leaves the pane present but
    /// zero points wide.
    private var isKindPaneVisible: Bool {
        guard let kindPane, !kindPane.isHidden else { return false }
        return kindPane.frame.width > 1
    }

    /// Width the file-kind panel needs to show every column in full.
    ///
    /// Computed from the columns rather than fixed, so it stays correct if a
    /// column is resized or the small-font setting changes the layout.
    private var kindPaneRequiredWidth: CGFloat {
        let columns = kindTable.tableColumns
        guard !columns.isEmpty else { return 300 }
        let content = columns.reduce(0) { $0 + $1.width }
        let spacing = kindTable.intercellSpacing.width * CGFloat(columns.count - 1)
        // Allowance for the scroller and the table's edge insets.
        return ceil(content + spacing + 26)
    }

    private func updateSidePanelButton() {
        let visible = isKindPaneVisible
        treePane.sidePanelButton.image = visible ? Assets.sideHide : Assets.sideShow
        treePane.sidePanelButton.toolTip = visible ? "Hide file kinds" : "Show file kinds"
    }

    @objc private func toggleSidePanel(_ sender: Any?) {
        if isKindPaneVisible {
            Preferences.shared.showFileKindStatistics = false
        } else if Preferences.shared.showFileKindStatistics {
            // Present but dragged shut, so the setting is already on and changing
            // it would notify nothing. Restore the width directly.
            splitView.setPosition(kindPaneRequiredWidth, ofDividerAt: 0)
            updateSidePanelButton()
        } else {
            // applyPreferences reopens it at the width its columns need.
            Preferences.shared.showFileKindStatistics = true
        }
    }

    @objc private func splitViewResized(_ note: Notification) {
        updateSidePanelButton()
    }

    /// Returns the tree to how it looked when the scan finished: everything shut
    /// except the root.
    @objc private func collapseAll(_ sender: Any?) {
        treeController.collapseToRoot()
        treeController.refresh()
        treePane.reloadPinnedColumn()
    }

    @objc private func zoomInToSelection() {
        guard let node = treeController.selectedNode(), let arena = session.arena,
            arena.isDirectory(Int(node))
        else { return }
        treemapView.zoom(toNode: node)
    }

    @objc private func zoomTreemapHere() {
        guard let node = treeController.selectedNode() else { return }
        treemapView.zoom(toNode: node)
    }

    @objc private func revealInFinder() {
        guard let node = treeController.selectedNode(), let arena = session.arena else { return }
        NSWorkspace.shared.activateFileViewerSelecting([
            URL(fileURLWithPath: arena.path(Int(node)))
        ])
    }

    @objc private func copyPath() {
        guard let node = treeController.selectedNode(), let arena = session.arena else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(arena.path(Int(node)), forType: .string)
    }

    @objc private func moveToTrash() {
        guard let node = treeController.selectedNode(), let arena = session.arena else { return }
        let path = arena.path(Int(node))
        let alert = NSAlert()
        alert.messageText = "Move to Trash?"
        alert.informativeText = path
        alert.addButton(withTitle: "Move to Trash")
        alert.addButton(withTitle: "Cancel")
        alert.alertStyle = .warning
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        do {
            try FileManager.default.trashItem(
                at: URL(fileURLWithPath: path), resultingItemURL: nil)
            // The bytes are gone from the disk, so they go out of every figure
            // that counted them. The row itself stays, showing zero.
            session.remove(node: node)
            treemapView.highlightedNode = nil
            refresh()
            updateInspectors(for: node)
        } catch {
            let failure = NSAlert(error: error)
            failure.runModal()
        }
    }

    /// The size to show, honouring the physical/logical setting. Only the total has
    /// both forms; the recency figures are accumulated from occupied space alone.
    func displaySize(_ arena: NodeArena, _ node: Int) -> Int64 {
        if !Preferences.shared.showPhysicalFileSize, session.metric == .total {
            return arena.logical[node]
        }
        return arena.size(session.metric, node)
    }

    /// Pushes the current selection to the Information and Selection panels.
    private func updateInspectors(for node: Int32) {
        guard let arena = session.arena, node >= 0, Int(node) < arena.count else { return }
        DebugLog.dumpSelection(
            node: node, arena: arena, image: treemapView.currentImage, palette: palette,
            metric: session.metric, displaySize: displaySize(arena, Int(node)))
        InformationPanelController.shared.update(
            arena: arena, node: Int(node),
            sizeText: Format.bytes(displaySize(arena, Int(node))))
        SelectionListPanelController.shared.update(arena: arena, node: node) { index in
            Format.bytes(self.displaySize(arena, index))
        }
    }

    // MARK: - Menu commands

    func showPreferences(_ sender: Any?) { PreferencesWindowController.shared.show() }
    func openFolder(_ sender: Any?) { showWelcome() }
    func zoomIn(_ sender: Any?) { zoomInToSelection() }
    func zoomOut(_ sender: Any?) { treemapView.zoomOut() }

    func zoomOutTo(_ sender: Any?) {
        guard let item = sender as? NSMenuItem else { return }
        // Pop back to the chosen level, then let the normal zoom-out animation run
        // for the final step so the motion still reads as leaving a folder.
        let target = Int32(item.tag)
        while treemapView.canZoomOut, treemapView.zoomedNode != target {
            treemapView.zoomOut()
        }
    }

    func selectParentFolder(_ sender: Any?) {
        guard let arena = session.arena, let node = treeController.selectedNode(), node != 0
        else { return }
        let parent = arena.parent[Int(node)]
        treeController.reveal(parent)
        treemapView.highlightedNode = parent
    }

    func toggleFreeSpace(_ sender: Any?) {
        Preferences.shared.showFreeSpace.toggle()
    }
    func toggleOtherSpace(_ sender: Any?) {
        Preferences.shared.showOtherSpace.toggle()
    }
    func togglePackageContents(_ sender: Any?) {
        Preferences.shared.showPackageContents.toggle()
    }
    func toggleLogicalSize(_ sender: Any?) {
        // The menu offers the logical size, the setting stores the physical one.
        Preferences.shared.showPhysicalFileSize.toggle()
    }
    func toggleSplitHorizontally(_ sender: Any?) {
        Preferences.shared.splitHorizontally.toggle()
    }
    func toggleKindStatistics(_ sender: Any?) {
        Preferences.shared.showFileKindStatistics.toggle()
    }
    func toggleSelectionList(_ sender: Any?) {
        Preferences.shared.showSelectionList.toggle()
    }

    /// Fills the "Zoom Out To" submenu from the current zoom stack.
    func menuNeedsUpdate(_ menu: NSMenu) {
        guard menu.title == "Zoom Out To" else { return }
        menu.removeAllItems()
        guard let arena = session.arena else { return }
        let stack = treemapView.zoomStack
        guard stack.count > 1 else {
            let empty = NSMenuItem(title: "Not Zoomed In", action: nil, keyEquivalent: "")
            empty.isEnabled = false
            menu.addItem(empty)
            return
        }
        for entry in stack.dropLast() {
            let index = Int(entry.node)
            guard index < arena.count else { continue }
            let name = index == 0 ? session.rootPath : arena.name(index)
            let item = NSMenuItem(
                title: name, action: #selector(zoomOutTo(_:)), keyEquivalent: "")
            item.target = self
            item.tag = Int(entry.node)
            menu.addItem(item)
        }
    }

    // MARK: - Preferences

    @objc private func preferencesChanged() { applyPreferences() }

    private func applyPreferences() {
        let p = Preferences.shared

        splitView.isVertical = !p.splitHorizontally
        kindPane.isHidden = !p.showFileKindStatistics
        // Reopening restores a width that fits every column, rather than whatever
        // it happened to be when it was closed.
        if p.showFileKindStatistics, !kindPaneWasVisible {
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.splitView.setPosition(self.kindPaneRequiredWidth, ofDividerAt: 0)
                self.updateSidePanelButton()
            }
        }
        kindPaneWasVisible = p.showFileKindStatistics
        updateSidePanelButton()

        palette.useConsistentColours = p.consistentKindColours
        treemapView.animatesZoom = p.animatedZooming

        var options = TreemapOptions()
        options.useLogicalSize = !p.showPhysicalFileSize
        options.expandPackages = p.showPackageContents
        options.extraBlocks = extraBlocks()
        treemapView.renderOptions = options

        kindController.smallFont = p.smallFontKindStatistics
        treeController.smallFont = p.smallFontFilesView

        if p.showSelectionList { SelectionListPanelController.shared.show() } else {
            SelectionListPanelController.shared.hide()
        }
        // The information panel has no way in any more, so it stays shut. A value
        // left over from when the View menu could open it would otherwise bring
        // it back on the next preference change of any kind.
        InformationPanelController.shared.hide()

        refresh()
    }

    /// The free-space and other-space blocks, when enabled and meaningful.
    private func extraBlocks() -> [ExtraBlock] {
        let p = Preferences.shared
        guard session.volumeCapacity > 0, let arena = session.arena, arena.count > 0 else {
            return []
        }
        var blocks: [ExtraBlock] = []
        if p.showFreeSpace, session.volumeFree > 0 {
            blocks.append(
                ExtraBlock(
                    id: -2, label: "Free space", bytes: session.volumeFree,
                    colour: 0xFF3C_4450))
        }
        // Meaningless when the whole drive is on screen: there is nothing else.
        if p.showOtherSpace, !session.scanningWholeVolume {
            let used = session.volumeCapacity - session.volumeFree
            let other = used - arena.allocated[0]
            if other > 0 {
                blocks.append(
                    ExtraBlock(id: -3, label: "Other space", bytes: other, colour: 0xFF5A_5252))
            }
        }
        return blocks
    }

    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        let p = Preferences.shared
        switch menuItem.action {
        case #selector(toggleFreeSpace(_:)):
            menuItem.state = p.showFreeSpace ? .on : .off
            return session.volumeCapacity > 0
        case #selector(toggleOtherSpace(_:)):
            menuItem.state = p.showOtherSpace ? .on : .off
            return session.volumeCapacity > 0 && !session.scanningWholeVolume
        case #selector(togglePackageContents(_:)):
            menuItem.state = p.showPackageContents ? .on : .off
            return true
        case #selector(toggleLogicalSize(_:)):
            menuItem.state = p.showPhysicalFileSize ? .off : .on
            return true
        case #selector(toggleSplitHorizontally(_:)):
            menuItem.state = p.splitHorizontally ? .on : .off
            return true
        case #selector(toggleKindStatistics(_:)):
            menuItem.state = p.showFileKindStatistics ? .on : .off
            return true
        case #selector(toggleSelectionList(_:)):
            menuItem.state = p.showSelectionList ? .on : .off
            return true
        case #selector(zoomIn(_:)):
            return treeController.selectedNode().map { node in
                session.arena?.isDirectory(Int(node)) ?? false
            } ?? false
        case #selector(zoomOut(_:)):
            return treemapView.canZoomOut
        case #selector(selectParentFolder(_:)):
            return (treeController.selectedNode() ?? 0) != 0
        case #selector(revealInFinder), #selector(copyPath), #selector(zoomTreemapHere):
            return treeController.selectedNode() != nil
        case #selector(moveToTrash):
            // Removing an entry rewrites the totals, which only one thing may do
            // at a time.
            return !session.isScanning && treeController.selectedNode() != nil
        case #selector(refreshFolder):
            // Only folders, and only when nothing else is already walking the
            // disk — two writers into one arena is exactly what this design
            // does not allow.
            guard !session.isScanning, let node = treeController.selectedNode(),
                let arena = session.arena
            else { return false }
            return arena.isDirectory(Int(node))
        case #selector(refreshEverything):
            return !session.isScanning && !session.rootPath.isEmpty
        default:
            return true
        }
    }

    // MARK: TreemapViewDelegate

    func treemapView(_ view: TreemapView, didSelect node: Int32) {
        treeController.reveal(node)
        view.highlightedNode = node
    }

    func treemapView(_ view: TreemapView, didZoomTo node: Int32) {
        zoomOutButton.isHidden = !view.canZoomOut
    }

    func treemapView(
        _ view: TreemapView, showTooltip content: (title: String, detail: String)?,
        at windowPoint: CGPoint
    ) {
        guard let content, let container = window.contentView else {
            tooltipView.hide()
            return
        }
        tooltipView.show(
            title: content.title, detail: content.detail, near: windowPoint, in: container)
    }
}

// MARK: - Application

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var controller: MainWindowController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // AppKit adds "Show Tab Bar" and "Show All Tabs" to the View menu for any
        // app that permits window tabbing. This app shows one scan per window and
        // has no use for tabs, so the commands only add clutter.
        NSWindow.allowsAutomaticWindowTabbing = false
        let c = MainWindowController()
        controller = c
        c.show()
        if let path = CommandLine.arguments.dropFirst().first {
            c.scan(path: (path as NSString).expandingTildeInPath)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

/// Headless rendering path, so the treemap engine can be verified without a
/// window server: `DiskSpaceApp --render <path> <out.png>`.
func renderHeadless(path: String, output: String) {
    var opts = ScanOptions()
    opts.workers = 4
    let scanner = TreeScanner(options: opts)
    let r = scanner.scan(root: path)
    let arena = r.arena
    print("scanned \(Format.count(arena.count)) nodes, \(Format.bytes(arena.allocated[0]))")

    let width = ProcessInfo.processInfo.environment["W"].flatMap(Int.init) ?? 1400
    let height = ProcessInfo.processInfo.environment["H"].flatMap(Int.init) ?? 1000
    let palette = KindPalette()
    let ps = ProcessInfo.processInfo.environment["PS"].flatMap(Double.init) ?? 1
    var baseOptions = TreemapOptions()
    baseOptions.pixelScale = ps
    var rendered = Treemap.render(
        arena: arena, root: 0, width: width, height: height, palette: palette,
        options: baseOptions)
    for m in SizeMetric.allCases {
        let started = Date()
        var options = baseOptions
        options.metric = m
        let img = Treemap.render(
            arena: arena, root: 0, width: width, height: height, palette: palette,
            options: options)
        let ms = Date().timeIntervalSince(started) * 1000
        print(String(format: "  %-26@ %11@  %6@ cells  %5.1f ms", m.title as NSString,
                     Format.bytes(arena.size(m, 0)) as NSString,
                     Format.count(img.cells.count) as NSString, ms))
        if m == .total { rendered = img }
    }

    // Confirm the ID buffer agrees with the geometry it was built from.
    // What is actually on screen, by pixel area — the only measure that matches
    // what the eye sees.
    var areaByReason: [String: Int] = [:]
    var dirLeafArea = 0
    for cell in rendered.cells where cell.isLeaf {
        let a = max(0, Int(cell.rect.width.rounded())) * max(0, Int(cell.rect.height.rounded()))
        areaByReason[cell.leafReason.rawValue, default: 0] += a
        if arena.isDirectory(Int(cell.node)) { dirLeafArea += a }
    }
    let totalPixels = width * height
    print("\nleaf pixel area by reason (of \(Format.count(totalPixels)) px):")
    for (reason, area) in areaByReason.sorted(by: { $0.value > $1.value }) {
        print(String(format: "  %-16@ %8@ px  %5.1f%%", reason as NSString,
                     Format.count(area) as NSString, Double(area) / Double(totalPixels) * 100))
    }
    print(String(format: "  drawn as a folder block: %.1f%% of pixels",
                 Double(dirLeafArea) / Double(totalPixels) * 100))
    let uncovered = rendered.ids.reduce(0) { $1 < 0 ? $0 + 1 : $0 }
    print(String(format: "  background showing through: %@ px (%.1f%%)",
                 Format.count(uncovered) as NSString,
                 Double(uncovered) / Double(totalPixels) * 100))

    // What the biggest blocks on screen actually are, and what the hover logic
    // would call them.
    let biggest = rendered.cells.enumerated()
        .filter { $0.element.isLeaf }
        .sorted { ($0.element.rect.width * $0.element.rect.height)
            > ($1.element.rect.width * $1.element.rect.height) }
        .prefix(8)
    print("\nlargest blocks on screen:")
    for (_, cell) in biggest {
        let n = Int(cell.node)
        let area = Int(cell.rect.width * cell.rect.height)
        let kindName = FileKind.names[Int(arena.kind[n])]
        let kindOfBlock = arena.isDirectory(n) ? "DIR" : "file"
        let pooled = cell.aggregatedCount > 0 ? " pooled=\(cell.aggregatedCount)" : ""
        print("  \(area) px  \(kindOfBlock)  [\(kindName)]  \(Format.bytes(arena.size(.total, n)))\(pooled)")
        print("      \(arena.path(n))")
        // Walk the ancestor chain with each one's drawn rectangle.
        var chain: [String] = []
        var cur = arena.isDirectory(n) ? n : Int(arena.parent[n])
        var hops = 0
        while cur > 0, hops < 6 {
            let r = rendered.directoryRects[Int32(cur)]
            let a = r.map { Int($0.width * $0.height) } ?? -1
            chain.append("\(arena.name(cur))(\(a)px)")
            cur = Int(arena.parent[cur])
            hops += 1
        }
        print("      ancestors: \(chain.joined(separator: " < "))")
    }

    // How stable is the resolved block across a large uniform-looking region?
    // Sample a grid and count how many distinct blocks each big cell's neighbourhood
    // resolves to.
    if let biggestCell = rendered.cells.enumerated().filter({ $0.element.isLeaf })
        .max(by: { ($0.element.rect.width * $0.element.rect.height)
            < ($1.element.rect.width * $1.element.rect.height) })
    {
        let r = biggestCell.element.rect
        print("\nstability sweep across the largest block's rect (\(Int(r.width))x\(Int(r.height))):")
        var seenCells = Set<Int32>()
        var seenParents = Set<Int>()
        var step = 0
        while step < 40 {
            let fx = r.minX + r.width * Double(step) / 40.0
            let fy = r.minY + r.height * 0.5
            let id = rendered.cellIndex(atX: Int(fx), y: Int(fy))
            if id >= 0 {
                seenCells.insert(id)
                let n = Int(rendered.cells[Int(id)].node)
                seenParents.insert(arena.isDirectory(n) ? n : Int(arena.parent[n]))
            }
            step += 1
        }
        print("  distinct cells across that span: \(seenCells.count)")
        print("  distinct enclosing folders:      \(seenParents.count)")
        for p in seenParents.prefix(6) { print("    \(arena.path(p))") }
    }

    // Trace a node by name: what the list would colour it, where the map puts it,
    // and what is actually drawn at that spot.
    if let needle = ProcessInfo.processInfo.environment["TRACE"] {
        print("\ntrace '\(needle)':")
        var found = 0
        for i in 1..<arena.count where found < 3 {
            let name = arena.name(i)
            guard name.contains(needle) else { continue }
            found += 1
            let kind = arena.kind[i]
            print("  node \(i) '\(name)'")
            print("    kind \(kind) = \(FileKind.names[Int(kind)])  dir=\(arena.isDirectory(i))")
            guard let r = rendered.directoryRects[Int32(i)] else {
                print("    NO RECT in directoryRects")
                continue
            }
            print(String(format: "    rect x %.1f..%.1f y %.1f..%.1f (%.0f px)",
                         r.minX, r.maxX, r.minY, r.maxY, r.width * r.height))
            let cx = Int(r.midX), cy = Int(r.midY)
            let id = rendered.cellIndex(atX: cx, y: cy)
            if id < 0 { print("    id buffer at centre: none"); continue }
            let painted = rendered.cells[Int(id)]
            let pn = Int(painted.node)
            print("    id buffer at centre -> cell \(id), node \(pn) '\(pn >= 0 ? arena.name(pn) : "extra")'")
            if pn >= 0 {
                print("    that node's kind = \(FileKind.names[Int(arena.kind[pn])])")
            }
            if pn != i { print("    *** MISMATCH: highlight rect does not contain this node ***") }
            // Write a magnified crop around the rect so the neighbourhood can be
            // inspected rather than inferred.
            if let img = rendered.image {
                let pad = 90.0
                let crop = CGRect(
                    x: max(0, r.minX - pad), y: max(0, r.minY - pad),
                    width: min(Double(width) - max(0, r.minX - pad), r.width + pad * 2),
                    height: min(Double(height) - max(0, r.minY - pad), r.height + pad * 2))
                if let piece = img.cropping(to: crop) {
                    let scale = 6
                    let w = piece.width * scale, h = piece.height * scale
                    if let ctx = CGContext(
                        data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue)
                    {
                        ctx.interpolationQuality = .none
                        ctx.draw(piece, in: CGRect(x: 0, y: 0, width: w, height: h))
                        // Mark where the highlight would be drawn.
                        ctx.setStrokeColor(NSColor.systemYellow.cgColor)
                        ctx.setLineWidth(3)
                        ctx.stroke(
                            CGRect(
                                x: (r.minX - crop.minX) * Double(scale),
                                y: (r.minY - crop.minY) * Double(scale),
                                width: r.width * Double(scale), height: r.height * Double(scale)))
                        if let out = ctx.makeImage() {
                            let rep = NSBitmapImageRep(cgImage: out)
                            if let png = rep.representation(using: .png, properties: [:]) {
                                try? png.write(to: URL(fileURLWithPath: "/tmp/crop.png"))
                                print("    wrote /tmp/crop.png")
                            }
                        }
                    }
                }
            }

            // What the list swatch would use, versus what the map actually painted.
            let expected = palette.colour(forKind: arena.kind[i])
            print(String(format: "    list swatch colour  #%06X", expected & 0xFFFFFF))
            if let img = rendered.image, let raw = img.dataProvider?.data as Data? {
                let off = cy * img.bytesPerRow + cx * 4
                if off + 3 < raw.count {
                    print(String(format: "    painted at centre   #%02X%02X%02X",
                                 raw[off + 2], raw[off + 1], raw[off]))
                }
            }
        }
        if found == 0 { print("  no match") }
    }

    // Every top-level child rect, and whether they tile the map without gaps.
    print("\ntop-level layout:")
    var covered = 0.0
    var rects: [(String, CGRect)] = []
    for k in arena.children(of: 0) {
        guard let r = rendered.directoryRects[Int32(k)] else { continue }
        rects.append((arena.name(k), r))
        covered += r.width * r.height
    }
    for (name, r) in rects.sorted(by: { $0.1.minX < $1.1.minX }) {
        print(String(format: "  %-24@ x %6.1f..%6.1f  y %6.1f..%6.1f", name as NSString,
                     r.minX, r.maxX, r.minY, r.maxY))
    }
    print(String(format: "  children cover %.3f%% of the map", covered / Double(width * height) * 100))

    // Is there a drawn border exactly on a top-level folder's edge?
    if let img = rendered.image, let raw = img.dataProvider?.data as Data? {
        let bpr = img.bytesPerRow
        func lum(_ x: Int, _ y: Int) -> Double {
            let off = y * bpr + x * 4
            guard off + 3 < raw.count else { return -1 }
            return 0.2126 * Double(raw[off + 2]) + 0.7152 * Double(raw[off + 1])
                + 0.0722 * Double(raw[off])
        }
        for k in arena.children(of: 0)
            .sorted(by: { arena.size(.total, $0) > arena.size(.total, $1) }).prefix(3)
        {
            guard let r = rendered.directoryRects[Int32(k)] else { continue }
            let edgeX = Int(r.maxX.rounded()) - 1
            let midY = Int(r.midY)
            var profile: [String] = []
            for d in -4...4 {
                let v = lum(edgeX + d, midY)
                profile.append(v < 0 ? "--" : String(format: "%.0f", v))
            }
            print("  \(arena.name(k)) rect x \(Int(r.minX))..\(Int(r.maxX)) y \(Int(r.minY))..\(Int(r.maxY))  right-edge profile [\(profile.joined(separator: " "))]")
        }
    }

    // Does the rectangle we would highlight actually contain that folder's content?
    print("\nhighlight rect vs what is drawn inside it:")
    func isDescendant(_ node: Int, of ancestor: Int) -> Bool {
        var cur = node
        var hops = 0
        while cur > 0, hops < 512 {
            if cur == ancestor { return true }
            cur = Int(arena.parent[cur])
            hops += 1
        }
        return cur == ancestor
    }
    let kids = arena.children(of: 0)
        .sorted { arena.size(.total, $0) > arena.size(.total, $1) }.prefix(5)
    for k in kids {
        guard let r = rendered.directoryRects[Int32(k)] else { continue }
        var inside = 0
        var outside = 0
        for i in 0..<12 {
            for j in 0..<12 {
                let sx = Int(r.minX + r.width * (Double(i) + 0.5) / 12)
                let sy = Int(r.minY + r.height * (Double(j) + 0.5) / 12)
                let id = rendered.cellIndex(atX: sx, y: sy)
                guard id >= 0 else { continue }
                if isDescendant(Int(rendered.cells[Int(id)].node), of: k) { inside += 1 }
                else { outside += 1 }
            }
        }
        print(String(format: "  %-22@ %5.1f%% of map   samples inside %3d  FOREIGN %3d",
                     arena.name(k) as NSString,
                     r.width * r.height / Double(width * height) * 100, inside, outside))
    }
    // Walk down a vertical line and report where intensity jumps, which is what a
    // visible border looks like.
    if let px = rendered.image, let data = px.dataProvider?.data as Data? {
        let bpr = px.bytesPerRow
        let x = width / 4
        var previous = -1.0
        var jumps: [(Int, Double)] = []
        for y in 1..<height {
            let off = y * bpr + x * 4
            guard off + 3 < data.count else { break }
            let b = Double(data[off]), g = Double(data[off + 1]), r = Double(data[off + 2])
            let lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
            if previous >= 0 {
                let delta = abs(lum - previous)
                if delta > 12 { jumps.append((y, delta)) }
            }
            previous = lum
        }
        print("  vertical scan at x=\(x): \(jumps.count) intensity steps > 12/255")
        for (y, d) in jumps.prefix(12) { print(String(format: "     y=%4d  step %.0f", y, d)) }
    }

    var probes = 0, unresolved = 0, mismatches = 0
    for (index, cell) in rendered.cells.enumerated() where cell.isLeaf {
        let cx = Int(cell.rect.midX), cy = Int(cell.rect.midY)
        guard cx < width, cy < height, cell.rect.width >= 1, cell.rect.height >= 1 else { continue }
        probes += 1
        let id = rendered.cellIndex(atX: cx, y: cy)
        if id < 0 { unresolved += 1 } else if Int(id) != index { mismatches += 1 }
    }
    print("id-buffer: \(Format.count(probes)) probes, \(unresolved) unresolved, \(mismatches) resolving to the wrong cell")

    guard let image = rendered.image else { print("no image"); exit(1) }
    let rep = NSBitmapImageRep(cgImage: image)
    guard let png = rep.representation(using: .png, properties: [:]) else {
        print("png encode failed"); exit(1)
    }
    try? png.write(to: URL(fileURLWithPath: output))
    print("wrote \(output)")
}

/// Prints exactly what the welcome screen will offer, so the list can be checked
/// without a window server.
func dumpSuggestions() {
    print("Full Disk Access: \(FullDiskAccess.isGranted ? "granted" : "NOT granted — banner shown")")
    print("\nVolumes:")
    for v in SuggestionSource.volumes() {
        let cap = v.capacity.map {
            "  \(Format.bytes($0.total - $0.free)) used of \(Format.bytes($0.total))"
        } ?? ""
        print("  • \(v.title)  [\(v.subtitle)]  \(v.path)\(cap)")
    }
    print("\nFolders:")
    for f in SuggestionSource.folders() {
        print("  • \(f.title)  [\(f.subtitle)]  \(f.path)")
    }
}

let rawArgs = Array(CommandLine.arguments.dropFirst())
if rawArgs.contains("--suggestions") {
    dumpSuggestions()
    exit(0)
}
if let i = rawArgs.firstIndex(of: "--render"), i + 2 < rawArgs.count {
    renderHeadless(
        path: (rawArgs[i + 1] as NSString).expandingTildeInPath, output: rawArgs[i + 2])
    exit(0)
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.activate(ignoringOtherApps: true)
app.run()
