import AppKit
import DiskScanCore

/// The "Pending Deletes" pane on the right, shown only while something is in it.
///
/// Each row is a name, the size it had, a pie chart draining through its ten
/// seconds, and a button to put it back. Two more buttons are pinned to the
/// bottom: one undoes all of them, one deletes them all now.
///
/// Drawn rather than assembled out of table cells. The list is short, every row
/// redraws ten times a second as its clock moves, and a view per row would be
/// more machinery than a list that never holds more than a handful.
final class PendingDeletesView: NSView {
    /// Fixed: the pane holds a name, a size and a button, and extra width would
    /// only stretch the gap between them.
    static let paneWidth: CGFloat = 300

    private static let headerHeight: CGFloat = 32
    private static let footerHeight: CGFloat = 44
    private static let rowHeight: CGFloat = 34
    private static let pieSize: CGFloat = 16

    var model: PendingDeletes?
    /// "Delete Now" was pressed. The window asks for confirmation and then calls
    /// flushAll: this view does not delete anything and puts up no dialog.
    var onDeleteNow: (() -> Void)?

    private let scroll = NSScrollView()
    private let list = ListView()
    private let undoAll = NSButton()
    private let deleteNow = NSButton()

    override init(frame: NSRect) {
        super.init(frame: frame)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    override var isFlipped: Bool { true }

    private func build() {
        translatesAutoresizingMaskIntoConstraints = false
        clipsToBounds = true

        let heading = NSTextField(labelWithString: "Pending Deletes")
        heading.font = .systemFont(ofSize: 12, weight: .semibold)
        heading.translatesAutoresizingMaskIntoConstraints = false

        list.owner = self
        list.translatesAutoresizingMaskIntoConstraints = false
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.documentView = list

        undoAll.title = "Undelete All"
        undoAll.bezelStyle = .rounded
        undoAll.target = self
        undoAll.action = #selector(undoEverything)
        undoAll.translatesAutoresizingMaskIntoConstraints = false

        // Red, because it is the one thing in this pane that cannot be undone.
        deleteNow.title = "Delete Now"
        deleteNow.bezelStyle = .rounded
        deleteNow.target = self
        deleteNow.action = #selector(deleteEverything)
        deleteNow.translatesAutoresizingMaskIntoConstraints = false
        deleteNow.contentTintColor = .white
        deleteNow.bezelColor = NSColor.systemRed

        let separator = NSBox()
        separator.boxType = .separator
        separator.translatesAutoresizingMaskIntoConstraints = false

        addSubview(heading)
        addSubview(scroll)
        addSubview(separator)
        addSubview(undoAll)
        addSubview(deleteNow)

        NSLayoutConstraint.activate([
            heading.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            heading.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),

            scroll.topAnchor.constraint(equalTo: topAnchor, constant: Self.headerHeight),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: separator.topAnchor),

            separator.leadingAnchor.constraint(equalTo: leadingAnchor),
            separator.trailingAnchor.constraint(equalTo: trailingAnchor),
            separator.bottomAnchor.constraint(
                equalTo: bottomAnchor, constant: -Self.footerHeight),

            undoAll.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            undoAll.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -12),
            deleteNow.leadingAnchor.constraint(equalTo: undoAll.trailingAnchor, constant: 8),
            deleteNow.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            deleteNow.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -12),
            deleteNow.widthAnchor.constraint(equalTo: undoAll.widthAnchor),
        ])
    }

    /// The list changed or a clock moved.
    func refresh() {
        let rows = model?.count ?? 0
        list.frame = NSRect(
            x: 0, y: 0, width: scroll.contentSize.width,
            height: max(scroll.contentSize.height, CGFloat(rows) * Self.rowHeight))
        list.needsDisplay = true
    }

    @objc private func undoEverything() {
        model?.undoAll()
        refresh()
    }

    @objc private func deleteEverything() {
        onDeleteNow?()
    }

    override func draw(_ dirty: NSRect) {
        NSColor.controlBackgroundColor.setFill()
        // Only ever this view's own area. Since macOS 14 a view does not clip to
        // its bounds by default, and the dirty rect handed in can be larger than
        // the view — filling it painted the pane's background over the whole
        // window, which is what left everything but this pane white.
        dirty.intersection(bounds).fill()
        NSColor.separatorColor.setFill()
        NSRect(x: 0, y: 0, width: 1, height: bounds.height).fill()
    }

    /// The rows themselves, so the scroll view has something to scroll.
    fileprivate final class ListView: NSView {
        weak var owner: PendingDeletesView?
        private var undoRects: [NSRect] = []

        override var isFlipped: Bool { true }

        override func draw(_ dirty: NSRect) {
            guard let items = owner?.model?.items else { return }
            let rowHeight = PendingDeletesView.rowHeight
            undoRects = Array(repeating: .zero, count: items.count)

            for (index, item) in items.enumerated() {
                let y = CGFloat(index) * rowHeight
                let row = NSRect(x: 0, y: y, width: bounds.width, height: rowHeight)
                guard row.intersects(dirty) else { continue }

                // The pie: blue while there is time, red as it runs out, crossing
                // over around half way. The curve is the model's, so the three
                // platforms agree about what half way looks like.
                let wedgeColour = NSColor.controlAccentColor.blended(
                    withFraction: CGFloat(item.urgency), of: .systemRed) ?? .controlAccentColor
                let pie = NSRect(
                    x: 12, y: y + (rowHeight - PendingDeletesView.pieSize) / 2,
                    width: PendingDeletesView.pieSize, height: PendingDeletesView.pieSize)
                NSColor.separatorColor.setFill()
                NSBezierPath(ovalIn: pie).fill()
                let path = NSBezierPath()
                let centre = NSPoint(x: pie.midX, y: pie.midY)
                path.move(to: centre)
                // Clockwise, starting from six o'clock: the same sweep half a
                // turn round from where it used to begin.
                path.appendArc(
                    withCenter: centre, radius: pie.width / 2, startAngle: -90,
                    endAngle: -90 - 360 * CGFloat(item.fraction), clockwise: true)
                path.close()
                wedgeColour.setFill()
                path.fill()

                let buttonWidth: CGFloat = 74
                let button = NSRect(
                    x: bounds.width - 12 - buttonWidth, y: y + (rowHeight - 20) / 2,
                    width: buttonWidth, height: 20)
                undoRects[index] = button
                NSColor.controlColor.setFill()
                let rounded = NSBezierPath(roundedRect: button, xRadius: 4, yRadius: 4)
                rounded.fill()
                NSColor.separatorColor.setStroke()
                rounded.stroke()
                draw("Undelete", in: button, align: .center, colour: .labelColor, size: 11)

                let sizeWidth: CGFloat = 66
                let nameLeft = 12 + PendingDeletesView.pieSize + 8
                let nameWidth = max(20, button.minX - 8 - sizeWidth - nameLeft)
                draw(
                    item.name,
                    in: NSRect(x: nameLeft, y: y, width: nameWidth, height: rowHeight),
                    align: .left, colour: .labelColor, size: 12)
                draw(
                    Format.bytes(item.bytes),
                    in: NSRect(
                        x: nameLeft + nameWidth, y: y, width: sizeWidth, height: rowHeight),
                    align: .right, colour: .secondaryLabelColor, size: 11)
            }
        }

        private func draw(
            _ text: String, in rect: NSRect, align: NSTextAlignment, colour: NSColor,
            size: CGFloat
        ) {
            let style = NSMutableParagraphStyle()
            style.alignment = align
            style.lineBreakMode = .byTruncatingMiddle
            let attributes: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: size),
                .foregroundColor: colour,
                .paragraphStyle: style,
            ]
            let height = NSFont.systemFont(ofSize: size).boundingRectForFont.height
            let box = NSRect(
                x: rect.minX, y: rect.midY - height / 2, width: rect.width, height: height)
            (text as NSString).draw(in: box, withAttributes: attributes)
        }

        override func mouseDown(with event: NSEvent) {
            let point = convert(event.locationInWindow, from: nil)
            for (index, rect) in undoRects.enumerated() where rect.contains(point) {
                owner?.model?.undo(at: index)
                owner?.refresh()
                return
            }
        }
    }
}
