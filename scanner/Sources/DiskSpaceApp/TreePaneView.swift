import AppKit

/// The file-tree pane: an outline view that may scroll horizontally, plus a Size
/// column pinned to the right edge.
///
/// A single table cannot do this — every column scrolls together, so deep
/// indentation eventually pushes Size out of view and forces the user to scroll
/// sideways or resize columns to read the number they came for. Keeping Size in a
/// separate table that only ever scrolls vertically makes it permanently visible,
/// while names stay free to be as wide as they need.
final class TreePaneView: NSView {
    let outlineView = NSOutlineView()
    let sizeTable = PinnedColumnTable()

    private let nameScroll = NSScrollView()
    private let sizeScroll = NSScrollView()
    private var syncing = false

    /// Supplies the text for a row of the pinned column.
    var sizeTextForRow: ((Int) -> String)?
    /// Supplies a row's display name, used to size the Name column to its content.
    var nameTextForRow: ((Int) -> String)?

    static let sizeColumnWidth: CGFloat = 118

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) { fatalError() }

    private func build() {
        translatesAutoresizingMaskIntoConstraints = false

        outlineView.rowHeight = 18
        outlineView.usesAlternatingRowBackgroundColors = true
        outlineView.indentationPerLevel = 13
        outlineView.headerView = NSTableHeaderView()
        // Names may exceed the pane; that is what the horizontal scroller is for.
        outlineView.columnAutoresizingStyle = .noColumnAutoresizing

        nameScroll.documentView = outlineView
        nameScroll.hasVerticalScroller = true
        nameScroll.hasHorizontalScroller = true
        nameScroll.autohidesScrollers = true
        nameScroll.translatesAutoresizingMaskIntoConstraints = false

        sizeTable.rowHeight = 18
        sizeTable.usesAlternatingRowBackgroundColors = true
        sizeTable.headerView = NSTableHeaderView()
        sizeTable.dataSource = self
        sizeTable.delegate = self
        sizeTable.selectionHighlightStyle = .regular
        // Pin the column so the table cannot autoresize it wider than the pane and
        // clip the last characters of the figure.
        sizeTable.columnAutoresizingStyle = .noColumnAutoresizing
        sizeTable.intercellSpacing = .zero
        let column = NSTableColumn(identifier: .init("size"))
        column.title = "Size"
        let columnWidth = Self.sizeColumnWidth - 12
        column.width = columnWidth
        column.minWidth = columnWidth
        column.maxWidth = columnWidth
        sizeTable.addTableColumn(column)

        sizeTable.forwardTo = nameScroll
        sizeScroll.documentView = sizeTable
        sizeScroll.hasVerticalScroller = false
        sizeScroll.hasHorizontalScroller = false
        sizeScroll.drawsBackground = false
        sizeScroll.translatesAutoresizingMaskIntoConstraints = false

        addSubview(nameScroll)
        addSubview(sizeScroll)
        NSLayoutConstraint.activate([
            nameScroll.topAnchor.constraint(equalTo: topAnchor),
            nameScroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            nameScroll.bottomAnchor.constraint(equalTo: bottomAnchor),
            nameScroll.trailingAnchor.constraint(equalTo: sizeScroll.leadingAnchor),
            sizeScroll.topAnchor.constraint(equalTo: topAnchor),
            sizeScroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            sizeScroll.bottomAnchor.constraint(equalTo: bottomAnchor),
            sizeScroll.widthAnchor.constraint(equalToConstant: Self.sizeColumnWidth),
        ])

        // Keep the pinned column's vertical offset locked to the outline's.
        nameScroll.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(
            self, selector: #selector(nameScrolled),
            name: NSView.boundsDidChangeNotification, object: nameScroll.contentView)

        // Expanding or collapsing changes how many rows exist, so the pinned column
        // has to be rebuilt to stay aligned.
        for note in [
            NSOutlineView.itemDidExpandNotification, NSOutlineView.itemDidCollapseNotification,
        ] {
            NotificationCenter.default.addObserver(
                self, selector: #selector(rowsChanged), name: note, object: outlineView)
        }
    }

    deinit { NotificationCenter.default.removeObserver(self) }

    @objc private func nameScrolled() {
        guard !syncing else { return }
        syncing = true
        let y = nameScroll.contentView.bounds.origin.y
        sizeScroll.contentView.scroll(to: CGPoint(x: 0, y: y))
        sizeScroll.reflectScrolledClipView(sizeScroll.contentView)
        syncing = false
    }

    @objc private func rowsChanged() {
        updateNameColumnWidth()
        sizeTable.reloadData()
        nameScrolled()
    }

    /// Call after the outline's own data changes.
    func reloadPinnedColumn() {
        updateNameColumnWidth()
        sizeTable.reloadData()
        nameScrolled()
    }

    override func layout() {
        super.layout()
        updateNameColumnWidth()
    }

    /// Sizes the Name column to the widest row currently on screen, but never
    /// narrower than the pane. The horizontal scroller therefore appears only when
    /// names genuinely need the room, and the pinned Size column is unaffected
    /// either way.
    private func updateNameColumnWidth() {
        guard let column = outlineView.tableColumns.first else { return }
        let available = nameScroll.contentSize.width
        guard available > 1 else { return }
        let font = NSFont.systemFont(ofSize: 11)
        var needed: CGFloat = 0
        let visible = outlineView.rows(in: nameScroll.contentView.documentVisibleRect)
        if visible.length > 0 {
            for row in visible.location..<(visible.location + visible.length) {
                guard row >= 0, row < outlineView.numberOfRows else { continue }
                let indent = CGFloat(outlineView.level(forRow: row) + 1)
                    * outlineView.indentationPerLevel
                let text = nameTextForRow?(row) ?? ""
                let width = (text as NSString).size(withAttributes: [.font: font]).width
                // Disclosure triangle, colour swatch and padding.
                needed = max(needed, indent + width + 48)
            }
        }
        let target = max(available, min(needed, 2000))
        if abs(column.width - target) > 0.5 { column.width = target }
    }

    func syncSelection() {
        let rows = outlineView.selectedRowIndexes
        if rows != sizeTable.selectedRowIndexes {
            sizeTable.selectRowIndexes(rows, byExtendingSelection: false)
        }
    }
}

extension TreePaneView: NSTableViewDataSource, NSTableViewDelegate {
    func numberOfRows(in tableView: NSTableView) -> Int { outlineView.numberOfRows }

    func tableView(
        _ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int
    ) -> NSView? {
        let cell = NSTableCellView()
        let text = NSTextField(labelWithString: sizeTextForRow?(row) ?? "")
        text.font = .systemFont(ofSize: 11)
        text.alignment = .right
        text.translatesAutoresizingMaskIntoConstraints = false
        cell.addSubview(text)
        cell.textField = text
        NSLayoutConstraint.activate([
            text.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 4),
            text.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -6),
            text.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
        ])
        return cell
    }

    /// Selection lives with the outline; clicking a size just selects its row there.
    func tableView(_ tableView: NSTableView, shouldSelectRow row: Int) -> Bool {
        guard row < outlineView.numberOfRows else { return false }
        outlineView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
        return true
    }
}

/// Scrolling over the pinned column should move the tree, not detach from it.
final class PinnedColumnTable: NSTableView {
    weak var forwardTo: NSScrollView?
    override func scrollWheel(with event: NSEvent) {
        forwardTo?.contentView.scrollWheel(with: event)
    }
}
