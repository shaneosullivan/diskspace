import AppKit
import DiskScanCore

/// Lists the chain of folders enclosing the current selection, with each one's
/// size, so the path being explored can be read at a glance and any level jumped
/// back to.
final class SelectionListPanelController: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    static let shared = SelectionListPanelController()

    private let panel: NSPanel
    private let tableView = NSTableView()
    private var entries: [(node: Int32, name: String, size: String, depth: Int)] = []

    /// Called when a row is chosen, so the main window can select that node.
    var onChoose: ((Int32) -> Void)?

    var isVisible: Bool { panel.isVisible }

    override init() {
        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 420, height: 300),
            styleMask: [.titled, .closable, .utilityWindow, .resizable],
            backing: .buffered, defer: false)
        panel.title = "Selection"
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        super.init()

        tableView.rowHeight = 20
        tableView.usesAlternatingRowBackgroundColors = true
        tableView.dataSource = self
        tableView.delegate = self
        tableView.headerView = NSTableHeaderView()

        let name = NSTableColumn(identifier: .init("name"))
        name.title = "Enclosing Folders"
        name.width = 280
        tableView.addTableColumn(name)
        let size = NSTableColumn(identifier: .init("size"))
        size.title = "Size"
        size.width = 100
        tableView.addTableColumn(size)

        let scroll = NSScrollView()
        scroll.documentView = tableView
        scroll.hasVerticalScroller = true
        panel.contentView = scroll
        panel.center()
    }

    func show() { panel.orderFront(nil) }
    func hide() { panel.orderOut(nil) }

    func clear() {
        entries = []
        tableView.reloadData()
    }

    /// Rebuilds the list for a node: the node itself, then each folder above it up
    /// to the scan root.
    func update(arena: NodeArena, node: Int32, sizeText: (Int) -> String) {
        guard panel.isVisible else { return }
        var chain: [Int] = []
        var current = Int(node)
        var hops = 0
        while current > 0, hops < 1024 {
            chain.append(current)
            current = Int(arena.parent[current])
            hops += 1
        }
        chain.append(0)

        entries = chain.reversed().enumerated().map { depth, index in
            (Int32(index), arena.name(index), sizeText(index), depth)
        }
        tableView.reloadData()
        if !entries.isEmpty {
            let last = entries.count - 1
            tableView.selectRowIndexes(IndexSet(integer: last), byExtendingSelection: false)
            tableView.scrollRowToVisible(last)
        }
    }

    func applyFont(small: Bool) {
        tableView.reloadData()
        _ = small
    }

    func numberOfRows(in tableView: NSTableView) -> Int { entries.count }

    func tableView(
        _ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int
    ) -> NSView? {
        guard row < entries.count, let id = tableColumn?.identifier.rawValue else { return nil }
        let entry = entries[row]
        let cell = NSTableCellView()
        let text = NSTextField(labelWithString: "")
        text.font = .systemFont(ofSize: Preferences.shared.fontSize(small: Preferences.shared.smallFontSelectionList))
        text.lineBreakMode = .byTruncatingMiddle
        text.translatesAutoresizingMaskIntoConstraints = false
        if id == "name" {
            // Indent by depth so the chain reads as a hierarchy.
            text.stringValue = String(repeating: "   ", count: entry.depth) + entry.name
        } else {
            text.stringValue = entry.size
            text.alignment = .right
        }
        cell.addSubview(text)
        cell.textField = text
        NSLayoutConstraint.activate([
            text.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 4),
            text.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -6),
            text.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
        ])
        return cell
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        let row = tableView.selectedRow
        guard row >= 0, row < entries.count else { return }
        onChoose?(entries[row].node)
    }
}
