import AppKit

/// Floating tooltip, added to the window's content view rather than to the treemap.
///
/// Drawn inside the treemap it was clipped at the pane boundary, so a tooltip near
/// the left edge lost its first characters behind the file list.
final class TooltipView: NSView {
    private let titleLabel = NSTextField(labelWithString: "")
    private let detailLabel = NSTextField(labelWithString: "")

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor(white: 0.09, alpha: 0.95).cgColor
        layer?.cornerRadius = 6
        layer?.shadowColor = NSColor.black.cgColor
        layer?.shadowOpacity = 0.35
        layer?.shadowRadius = 8
        layer?.shadowOffset = CGSize(width: 0, height: -2)

        titleLabel.font = .systemFont(ofSize: 12, weight: .semibold)
        titleLabel.textColor = .white
        titleLabel.lineBreakMode = .byTruncatingHead
        titleLabel.translatesAutoresizingMaskIntoConstraints = false

        detailLabel.font = .systemFont(ofSize: 11)
        detailLabel.textColor = NSColor(white: 0.78, alpha: 1)
        detailLabel.translatesAutoresizingMaskIntoConstraints = false

        addSubview(titleLabel)
        addSubview(detailLabel)
        NSLayoutConstraint.activate([
            titleLabel.topAnchor.constraint(equalTo: topAnchor, constant: 7),
            titleLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            titleLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),
            detailLabel.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 2),
            detailLabel.leadingAnchor.constraint(equalTo: titleLabel.leadingAnchor),
            detailLabel.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -10),
            detailLabel.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -7),
        ])
        isHidden = true
    }

    required init?(coder: NSCoder) { fatalError() }

    /// Ignore the pointer entirely, so the tooltip never blocks hovering or clicks
    /// on the map underneath it.
    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    private static let maximumWidth: CGFloat = 460

    func show(title: String, detail: String, near windowPoint: CGPoint, in container: NSView) {
        titleLabel.stringValue = title
        detailLabel.stringValue = detail

        let titleWidth = (title as NSString).size(
            withAttributes: [.font: titleLabel.font as Any]).width
        let detailWidth = (detail as NSString).size(
            withAttributes: [.font: detailLabel.font as Any]).width
        let width = min(Self.maximumWidth, max(titleWidth, detailWidth) + 20)
        let height: CGFloat = 44

        var origin = container.convert(windowPoint, from: nil)
        origin.x += 14
        origin.y -= height + 14
        // Keep it fully on screen, flipping side or edge rather than being clipped.
        let limits = container.bounds
        if origin.x + width > limits.maxX - 4 {
            origin.x = container.convert(windowPoint, from: nil).x - width - 14
        }
        origin.x = max(limits.minX + 4, origin.x)
        if origin.y < limits.minY + 4 {
            origin.y = container.convert(windowPoint, from: nil).y + 14
        }
        origin.y = min(limits.maxY - height - 4, origin.y)

        frame = CGRect(origin: origin, size: CGSize(width: width, height: height))
        isHidden = false
    }

    func hide() { isHidden = true }
}
