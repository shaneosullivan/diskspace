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

    init(tableView: NSTableView, palette: KindPalette) {
        self.tableView = tableView
        self.palette = palette
        super.init()
        tableView.dataSource = self
        tableView.delegate = self
    }

    func refresh() {
        rows = session?.scanner?.kindTotals(metric: session?.metric ?? .total) ?? []
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
        text.font = .systemFont(ofSize: 11)
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
        text.font = .systemFont(ofSize: 11)
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
        if row >= 0 {
            outlineView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
            outlineView.scrollRowToVisible(row)
        }
    }

    func selectedNode() -> Int32? {
        guard let outlineView, outlineView.selectedRow >= 0,
            let item = outlineView.item(atRow: outlineView.selectedRow)
        else { return nil }
        return node(from: item)
    }
}

// MARK: - Main window

final class MainWindowController: NSObject, TreemapViewDelegate, NSMenuItemValidation {
    private let window: NSWindow
    private let session = ScanSession()
    private let palette = KindPalette()

    private let kindTable = NSTableView()
    private let treePane = TreePaneView()
    private var outlineView: NSOutlineView { treePane.outlineView }
    private let treemapView = TreemapView()
    private let statusLabel = NSTextField(labelWithString: "")
    private let zoomOutButton = NSButton()
    private let metricPopUp = NSPopUpButton()

    private var kindController: KindTableController!
    private var treeController: TreeController!
    private var welcomeView: WelcomeView!
    private var splitView: NSSplitView!
    private let tooltipView = TooltipView()

    override init() {
        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1400, height: 880),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false)
        super.init()
        window.title = "Disk Space"
        window.center()
        buildUI()
        session.onRefresh = { [weak self] in self?.refresh() }
    }

    func show() {
        window.makeKeyAndOrderFront(nil)
        NotificationCenter.default.addObserver(
            self, selector: #selector(appBecameActive),
            name: NSApplication.didBecomeActiveNotification, object: nil)
    }

    @objc private func appBecameActive() {
        guard welcomeView != nil, !welcomeView.isHidden else { return }
        welcomeView.refreshAccessState()
    }

    func scan(path: String) {
        window.title = "Disk Space — \(path)"
        welcomeView.isHidden = true
        splitView.isHidden = false
        treemapView.setArena(nil)
        treeController.resetExpansion()
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
            toolbar.heightAnchor.constraint(equalToConstant: 36),
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

        DispatchQueue.main.async {
            split.setPosition(300, ofDividerAt: 0)
            split.setPosition(760, ofDividerAt: 1)
        }
    }

    private func makeToolbar() -> NSView {
        let bar = NSView()
        bar.translatesAutoresizingMaskIntoConstraints = false

        let openButton = NSButton(
            title: "Open…", target: self, action: #selector(chooseTarget))
        openButton.bezelStyle = .rounded
        openButton.translatesAutoresizingMaskIntoConstraints = false

        metricPopUp.translatesAutoresizingMaskIntoConstraints = false
        metricPopUp.target = self
        metricPopUp.action = #selector(metricChanged)
        for m in SizeMetric.allCases { metricPopUp.addItem(withTitle: m.title) }
        metricPopUp.selectItem(at: SizeMetric.total.rawValue)

        zoomOutButton.title = "Zoom Out"
        zoomOutButton.bezelStyle = .rounded
        zoomOutButton.target = self
        zoomOutButton.action = #selector(zoomOut)
        zoomOutButton.isHidden = true
        zoomOutButton.translatesAutoresizingMaskIntoConstraints = false

        statusLabel.font = .systemFont(ofSize: 11)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.translatesAutoresizingMaskIntoConstraints = false

        bar.addSubview(openButton)
        bar.addSubview(metricPopUp)
        bar.addSubview(statusLabel)
        bar.addSubview(zoomOutButton)
        NSLayoutConstraint.activate([
            openButton.leadingAnchor.constraint(equalTo: bar.leadingAnchor, constant: 10),
            openButton.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            metricPopUp.leadingAnchor.constraint(equalTo: openButton.trailingAnchor, constant: 10),
            metricPopUp.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            metricPopUp.widthAnchor.constraint(equalToConstant: 210),
            statusLabel.leadingAnchor.constraint(equalTo: metricPopUp.trailingAnchor, constant: 14),
            statusLabel.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            zoomOutButton.trailingAnchor.constraint(equalTo: bar.trailingAnchor, constant: -10),
            zoomOutButton.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
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
        colour.maxWidth = 18
        kindTable.addTableColumn(colour)

        let kind = NSTableColumn(identifier: .init("kind"))
        kind.title = "Kind"
        kind.width = 150
        kind.sortDescriptorPrototype = NSSortDescriptor(key: "kind", ascending: true)
        kindTable.addTableColumn(kind)

        let size = NSTableColumn(identifier: .init("size"))
        size.title = "Size"
        size.width = 72
        size.sortDescriptorPrototype = NSSortDescriptor(key: "size", ascending: false)
        kindTable.addTableColumn(size)

        let files = NSTableColumn(identifier: .init("files"))
        files.title = "Files"
        files.width = 66
        files.sortDescriptorPrototype = NSSortDescriptor(key: "files", ascending: false)
        kindTable.addTableColumn(files)

        kindTable.sortDescriptors = [NSSortDescriptor(key: "size", ascending: false)]
        kindController = KindTableController(tableView: kindTable, palette: palette)
        kindController.session = session
        return scrollWrapping(kindTable)
    }

    private func makeTreePane() -> NSView {
        let name = NSTableColumn(identifier: .init("name"))
        name.title = "Name"
        name.width = 320
        name.minWidth = 120
        outlineView.addTableColumn(name)
        outlineView.outlineTableColumn = name

        treePane.sizeTextForRow = { [weak self] row in
            guard let self, let arena = self.session.arena,
                let item = self.outlineView.item(atRow: row) as? NSNumber
            else { return "" }
            let node = Int(item.int32Value)
            guard node < arena.count else { return "" }
            return Format.bytes(arena.size(self.session.metric, node))
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
            self?.treemapView.needsDisplay = true
            _ = node
        }

        let menu = NSMenu()
        menu.addItem(
            NSMenuItem(title: "Reveal in Finder", action: #selector(revealInFinder), keyEquivalent: ""))
        menu.addItem(
            NSMenuItem(title: "Copy Path", action: #selector(copyPath), keyEquivalent: ""))
        menu.addItem(
            NSMenuItem(title: "Zoom Treemap Here", action: #selector(zoomTreemapHere), keyEquivalent: ""))
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

    private func refresh() {
        updateMetricAvailability()
        kindController.refresh()
        treeController.refresh()
        treemapView.refresh()
        zoomOutButton.isHidden = !treemapView.canZoomOut

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
            statusLabel.stringValue = "Scanning… \(counts)"
        } else {
            var line = "Scan complete · \(counts) · \(String(format: "%.1fs", session.elapsed))"
            if session.metric != .total {
                line += "  —  \(session.metric.title): \(Format.bytes(arena.size(session.metric, 0)))"
            }
            statusLabel.stringValue = line
        }
    }

    // MARK: Actions

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

    @objc private func zoomOut() { treemapView.zoomOut() }

    @objc private func zoomTreemapHere() {
        guard let node = treeController.selectedNode() else { return }
        treemapView.zoom(to: node)
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
        } catch {
            let failure = NSAlert(error: error)
            failure.runModal()
        }
    }

    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        treeController.selectedNode() != nil
    }

    // MARK: TreemapViewDelegate

    func treemapView(_ view: TreemapView, didSelect node: Int32) {
        treeController.reveal(node)
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

    let width = 1400, height = 1000
    let palette = KindPalette()
    var rendered = Treemap.render(
        arena: arena, root: 0, width: width, height: height, palette: palette)
    for m in SizeMetric.allCases {
        let started = Date()
        let img = Treemap.render(
            arena: arena, root: 0, width: width, height: height, palette: palette, metric: m)
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
