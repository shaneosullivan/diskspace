import AppKit
import DiskScanCore

/// Floating panel describing the current selection.
///
/// The scanner keeps only what a treemap needs, so ownership, permissions and
/// modification date are read with a single `stat` when the panel is shown. That
/// is one syscall per selection rather than eight bytes per node across millions.
final class InformationPanelController {
    static let shared = InformationPanelController()

    private let panel: NSPanel
    private let iconView = NSImageView()
    private let titleLabel = NSTextField(labelWithString: "")
    private let grid = NSGridView(numberOfColumns: 2, rows: 0)
    private var valueLabels: [String: NSTextField] = [:]

    private static let fields = [
        "Name", "Kind", "Modified", "Created", "Owner", "Group", "Permissions", "Path",
        "Application",
    ]

    var isVisible: Bool { panel.isVisible }

    private init() {
        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 460, height: 520),
            styleMask: [.titled, .closable, .utilityWindow, .resizable],
            backing: .buffered, defer: false)
        panel.title = "Information"
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.center()

        iconView.imageScaling = .scaleProportionallyUpOrDown
        iconView.translatesAutoresizingMaskIntoConstraints = false
        titleLabel.font = .systemFont(ofSize: 17, weight: .semibold)
        titleLabel.lineBreakMode = .byTruncatingMiddle
        titleLabel.translatesAutoresizingMaskIntoConstraints = false

        grid.translatesAutoresizingMaskIntoConstraints = false
        grid.columnSpacing = 8
        grid.rowSpacing = 6
        grid.column(at: 0).xPlacement = .trailing
        for field in Self.fields {
            let name = NSTextField(labelWithString: "\(field):")
            name.font = .systemFont(ofSize: 12, weight: .semibold)
            let value = NSTextField(wrappingLabelWithString: "—")
            value.font = .systemFont(ofSize: 12)
            value.isSelectable = true
            valueLabels[field] = value
            grid.addRow(with: [name, value])
        }

        let content = NSView()
        content.addSubview(iconView)
        content.addSubview(titleLabel)
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.documentView = grid
        scroll.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(scroll)

        NSLayoutConstraint.activate([
            iconView.topAnchor.constraint(equalTo: content.topAnchor, constant: 18),
            iconView.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
            iconView.widthAnchor.constraint(equalToConstant: 64),
            iconView.heightAnchor.constraint(equalToConstant: 64),
            titleLabel.leadingAnchor.constraint(equalTo: iconView.trailingAnchor, constant: 14),
            titleLabel.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -20),
            titleLabel.centerYAnchor.constraint(equalTo: iconView.centerYAnchor),
            scroll.topAnchor.constraint(equalTo: iconView.bottomAnchor, constant: 18),
            scroll.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 12),
            scroll.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -12),
            scroll.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -12),
            grid.widthAnchor.constraint(equalTo: scroll.widthAnchor, constant: -20),
        ])
        panel.contentView = content
    }

    func show() {
        panel.orderFront(nil)
    }

    func hide() {
        panel.orderOut(nil)
    }

    func clear() {
        titleLabel.stringValue = "No selection"
        iconView.image = nil
        for field in Self.fields { valueLabels[field]?.stringValue = "—" }
    }

    /// Updates the panel for a node. Does no work when the panel is hidden.
    func update(arena: NodeArena, node: Int, sizeText: String) {
        guard panel.isVisible, node < arena.count else { return }
        let path = arena.path(node)
        let url = URL(fileURLWithPath: path)

        titleLabel.stringValue = arena.name(node)
        iconView.image = NSWorkspace.shared.icon(forFile: path)

        var info = stat()
        let stated = path.withCString { lstat($0, &info) == 0 }

        let formatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .medium

        func date(_ seconds: Int) -> String {
            seconds > 0
                ? formatter.string(from: Date(timeIntervalSince1970: Double(seconds))) : "—"
        }

        set("Name", arena.name(node))
        set("Kind", kindDescription(arena: arena, node: node) + " · " + sizeText)
        set("Modified", stated ? date(info.st_mtimespec.tv_sec) : "—")
        set("Created", stated ? date(info.st_birthtimespec.tv_sec) : "—")
        set("Owner", stated ? userName(uid: info.st_uid) : "—")
        set("Group", stated ? groupName(gid: info.st_gid) : "—")
        set("Permissions", stated ? permissions(mode: info.st_mode) : "—")
        set("Path", path)

        if arena.isDirectory(node) {
            set("Application", "—")
        } else if let app = NSWorkspace.shared.urlForApplication(toOpen: url) {
            set("Application", app.lastPathComponent)
        } else {
            set("Application", "—")
        }
    }

    private func set(_ field: String, _ value: String) {
        valueLabels[field]?.stringValue = value
    }

    private func kindDescription(arena: NodeArena, node: Int) -> String {
        var kind = arena.kind[node]
        if kind == FileKind.directory {
            let bytes = arena.nameBytes(node)
            if let base = bytes.baseAddress, bytes.count > 0 {
                let named = FileKind.classify(
                    name: UnsafeRawPointer(base), length: bytes.count)
                if named != FileKind.noExtension, named != FileKind.unknown { kind = named }
            }
        }
        let index = Int(kind)
        guard index >= 0, index < FileKind.names.count else { return "Unknown" }
        return FileKind.names[index]
    }

    private func userName(uid: uid_t) -> String {
        guard let entry = getpwuid(uid), let name = entry.pointee.pw_name else { return "\(uid)" }
        return String(cString: name)
    }

    private func groupName(gid: gid_t) -> String {
        guard let entry = getgrgid(gid), let name = entry.pointee.gr_name else { return "\(gid)" }
        return String(cString: name)
    }

    /// Renders the mode the way `ls -l` does, with the octal alongside.
    private func permissions(mode: mode_t) -> String {
        let bits = mode & 0o777
        var text = ""
        for shift in [6, 3, 0] {
            let triple = (bits >> mode_t(shift)) & 0o7
            text += (triple & 0o4) != 0 ? "r" : "-"
            text += (triple & 0o2) != 0 ? "w" : "-"
            text += (triple & 0o1) != 0 ? "x" : "-"
        }
        return String(format: "-%@ (%03o)", text, bits)
    }
}
