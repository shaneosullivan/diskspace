import AppKit

/// The app icon in the menu bar, over by the clock, with a small badge on its
/// lower-left corner.
///
/// It answers one question from across the desk — are the figures on screen the
/// whole story yet? A scan of a large drive runs for minutes and people switch
/// away while it does. A red badge with a pulsing dot means the walk is still
/// going and the totals are still moving; green with a tick means it finished.
/// Stopped part way, the badge goes altogether — there is no longer anything
/// running to report on.
///
/// It appears when a scan starts and not before: with nothing being measured
/// there is nothing to report, and a permanent mark in the bar would only be one
/// more thing to ignore.
///
/// The item belongs to the app and has to be taken out of the menu bar by hand
/// when the app quits, or it lingers there with nothing behind it.
enum MenuBarStatus {
    enum State {
        /// A scan is running.
        case working
        /// A scan ran to the end.
        case done
    }

    static func show(_ state: State) {
        let item = item ?? make()
        current = state
        item.button?.toolTip =
            state == .working ? "Disk Space: scanning" : "Disk Space: scan complete"
        switch state {
        case .working: startPulse()
        case .done: stopPulse()
        }
        redraw()
    }

    static func clear() {
        stopPulse()
        guard let item else { return }
        NSStatusBar.system.removeStatusItem(item)
        self.item = nil
    }

    private static var item: NSStatusItem?
    private static var current: State = .working

    private static func make() -> NSStatusItem {
        let new = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        // Clicking it brings the window forward, which is the only thing anyone
        // would want from a badge that reports on a window.
        new.button?.target = NSApp
        new.button?.action = #selector(NSApplication.unhide(_:))
        item = new
        return new
    }

    // MARK: - The pulse

    /// One grow-and-shrink, in seconds. Slow enough to read as breathing rather
    /// than blinking.
    private static let pulsePeriod: TimeInterval = 1.6

    /// Redraws a second. The dot moves smoothly at this rate, and the menu bar
    /// is not somewhere to spend more of a battery than that.
    private static let pulseRate: TimeInterval = 15

    private static var pulseTimer: Timer?
    private static var pulseStarted = Date()

    private static func startPulse() {
        guard pulseTimer == nil else { return }
        pulseStarted = Date()
        let t = Timer(timeInterval: 1 / pulseRate, repeats: true) { _ in redraw() }
        RunLoop.main.add(t, forMode: .common)
        pulseTimer = t
    }

    private static func stopPulse() {
        pulseTimer?.invalidate()
        pulseTimer = nil
    }

    /// How wide the dot is, as a fraction of the badge, right now.
    ///
    /// A cosine rather than a sawtooth: it leaves and arrives at rest, so the dot
    /// eases out of the centre and settles back into it instead of snapping.
    private static var pulseScale: CGFloat {
        let smallest: CGFloat = 0.14
        let largest: CGFloat = 0.8
        let phase = Date().timeIntervalSince(pulseStarted)
            .truncatingRemainder(dividingBy: pulsePeriod) / pulsePeriod
        let eased = (1 - cos(2 * .pi * phase)) / 2
        return smallest + (largest - smallest) * eased
    }

    // MARK: - Drawing

    private static func redraw() {
        item?.button?.image = badge(current)
    }

    /// The app icon with the badge on its lower-left corner.
    ///
    /// The icon stays the thing you recognise in the bar; the badge is the
    /// smaller mark on top of it that carries the state. A ring of transparency
    /// is cleared around the badge so it reads as sitting over the icon rather
    /// than merging into it, whatever the menu bar happens to be sitting on.
    ///
    /// Not a template image: the two colours are the whole point, and a template
    /// would flatten both to the menu bar's own foreground.
    private static func badge(_ state: State) -> NSImage {
        let side: CGFloat = 20
        let image = NSImage(size: NSSize(width: side, height: side))
        image.lockFocus()
        // The icon sits up and to the right, leaving the lower-left corner for
        // the badge to occupy rather than cover.
        let icon = NSRect(x: 3, y: 3, width: side - 3, height: side - 3)
        NSApp.applicationIconImage?.draw(
            in: icon, from: .zero, operation: .sourceOver, fraction: 1)

        let diameter = side * 0.5
        let badge = NSRect(x: 0, y: 0, width: diameter, height: diameter)

        if let context = NSGraphicsContext.current {
            context.compositingOperation = .clear
            NSBezierPath(ovalIn: badge.insetBy(dx: -1.2, dy: -1.2)).fill()
            context.compositingOperation = .sourceOver
        }

        switch state {
        case .working: NSColor.systemRed.setFill()
        case .done: NSColor.systemGreen.setFill()
        }
        NSBezierPath(ovalIn: badge).fill()

        switch state {
        case .working: drawPulse(in: badge)
        case .done: drawTick(in: badge)
        }

        image.unlockFocus()
        image.isTemplate = false
        return image
    }

    private static func drawPulse(in badge: NSRect) {
        let width = badge.width * pulseScale
        let dot = NSRect(
            x: badge.midX - width / 2, y: badge.midY - width / 2,
            width: width, height: width)
        NSColor.white.setFill()
        NSBezierPath(ovalIn: dot).fill()
    }

    private static func drawTick(in badge: NSRect) {
        func point(_ x: CGFloat, _ y: CGFloat) -> NSPoint {
            NSPoint(x: badge.minX + badge.width * x, y: badge.minY + badge.height * y)
        }
        let tick = NSBezierPath()
        tick.move(to: point(0.26, 0.52))
        tick.line(to: point(0.43, 0.32))
        tick.line(to: point(0.75, 0.68))
        tick.lineWidth = badge.width * 0.18
        tick.lineCapStyle = .round
        tick.lineJoinStyle = .round
        NSColor.white.setStroke()
        tick.stroke()
    }
}
