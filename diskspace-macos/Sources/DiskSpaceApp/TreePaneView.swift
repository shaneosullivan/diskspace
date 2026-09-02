import AppKit

/// The file-tree pane: an outline view that may scroll horizontally, plus a Size
/// column pinned to the right edge.
///
/// A single table cannot do this — every column scrolls together, so deep
/// indentation eventually pushes Size out of view and forces the user to scroll
/// sideways or resize columns to read the number they came for. Keeping Size in a
/// separate table that only ever scrolls vertically makes it permanently visible,
/// while names stay free to be as wide as they need.
/// An outline view that hands the delete keys to the window.
///
/// Windows binds Del to the same command through its accelerator table, which
/// applies wherever the focus is. Here it is the list's own key handling: a bare
/// backspace as a menu key equivalent would fire while typing in any text field
/// in the app, which is exactly the accident this feature cannot afford.
final class FileOutlineView: NSOutlineView {
    /// Called for backspace and forward delete, when something is selected.
    var onDeleteKey: (() -> Void)?

    override func keyDown(with event: NSEvent) {
        let deleteKeys: Set<UInt16> = [
            UInt16(NSDeleteCharacter), UInt16(NSDeleteFunctionKey),
        ]
        if let scalar = event.charactersIgnoringModifiers?.unicodeScalars.first,
            deleteKeys.contains(UInt16(scalar.value)), !selectedRowIndexes.isEmpty
        {
            onDeleteKey?()
            return
        }
        super.keyDown(with: event)
    }
}

final class TreePaneView: NSView {
    let outlineView = FileOutlineView()
    let sizeTable = PinnedColumnTable()
    /// Sits in the header row, left of the Name title.
    let collapseButton = NSButton()
    /// Shows or hides the file-kind panel. Left of the collapse button.
    let sidePanelButton = NSButton()

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

        collapseButton.image = Assets.collapse
        collapseButton.imagePosition = .imageOnly
        collapseButton.imageScaling = .scaleProportionallyDown
        collapseButton.isBordered = false
        collapseButton.toolTip = "Collapse all folders"
        collapseButton.translatesAutoresizingMaskIntoConstraints = false

        sidePanelButton.imagePosition = .imageOnly
        sidePanelButton.imageScaling = .scaleProportionallyDown
        sidePanelButton.isBordered = false
        sidePanelButton.translatesAutoresizingMaskIntoConstraints = false
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
        // Added last so they sit above the header they share a row with.
        addSubview(sidePanelButton)
        addSubview(collapseButton)
        NSLayoutConstraint.activate([
            nameScroll.topAnchor.constraint(equalTo: topAnchor),
            nameScroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            nameScroll.bottomAnchor.constraint(equalTo: bottomAnchor),
            nameScroll.trailingAnchor.constraint(equalTo: sizeScroll.leadingAnchor),
            sizeScroll.topAnchor.constraint(equalTo: topAnchor),
            sizeScroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            sizeScroll.bottomAnchor.constraint(equalTo: bottomAnchor),
            sizeScroll.widthAnchor.constraint(equalToConstant: Self.sizeColumnWidth),
            sidePanelButton.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 5),
            sidePanelButton.topAnchor.constraint(equalTo: topAnchor, constant: 4),
            sidePanelButton.widthAnchor.constraint(equalToConstant: 16),
            sidePanelButton.heightAnchor.constraint(equalToConstant: 16),
            collapseButton.leadingAnchor.constraint(
                equalTo: sidePanelButton.trailingAnchor, constant: 6),
            collapseButton.topAnchor.constraint(equalTo: topAnchor, constant: 4),
            collapseButton.widthAnchor.constraint(equalToConstant: 16),
            collapseButton.heightAnchor.constraint(equalToConstant: 16),
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

/// Header cell that leaves room at its leading edge for a control drawn over it.
final class InsetHeaderCell: NSTableHeaderCell {
    /// Distance from the column's leading edge to the start of the title.
    var leadingInset: CGFloat = 0

    /// Draws the title directly at `leadingInset` from the column's edge.
    ///
    /// Deferring to `super` here is unreliable: `NSTableHeaderCell` does not
    /// consistently route its title through `titleRect`, so insetting the frame
    /// handed to it landed the text somewhere between the requested position and
    /// the default one. Placing the string explicitly makes the offset exact.
    override func drawInterior(withFrame cellFrame: NSRect, in controlView: NSView) {
        let text = NSAttributedString(
            string: stringValue,
            attributes: [
                .font: font ?? NSFont.systemFont(ofSize: 11),
                // Matches the other column headers, which AppKit draws in the
                // primary label colour.
                .foregroundColor: NSColor.labelColor,
            ])
        let size = text.size()
        let x = cellFrame.minX + leadingInset
        guard x < cellFrame.maxX else { return }
        text.draw(
            at: NSPoint(x: x, y: cellFrame.midY - size.height / 2))
    }
}

/// Scrolling over the pinned column should move the tree, not detach from it.
final class PinnedColumnTable: NSTableView {
    weak var forwardTo: NSScrollView?
    override func scrollWheel(with event: NSEvent) {
        forwardTo?.contentView.scrollWheel(with: event)
    }
}
