import AppKit
import DiskScanCore

protocol TreemapViewDelegate: AnyObject {
    func treemapView(_ view: TreemapView, didSelect node: Int32)
    func treemapView(_ view: TreemapView, didZoomTo node: Int32)
    /// Content is nil when the pointer leaves the map. The point is in window
    /// coordinates so the tooltip can be placed over any pane.
    func treemapView(
        _ view: TreemapView, showTooltip content: (title: String, detail: String)?,
        at windowPoint: CGPoint)
}

/// Draws the treemap as a single blitted image and hit-tests through the ID buffer.
///
/// Layout and rasterisation happen on a background queue; the main thread only
/// ever blits a finished image, so neither a rescan nor a resize can stall input.
final class TreemapView: NSView {
    weak var delegate: TreemapViewDelegate?

    private var arena: NodeArena?
    private var palette = KindPalette()
    private var current: TreemapImage?
    private var hoveredCell: Int = -1
    private var renderGeneration = 0
    /// True while a render is on the background queue.
    private var isRendering = false
    /// Set when a render was asked for while one was already running.
    private var renderQueued = false

    // MARK: Live refresh pacing
    //
    // The map is the expensive half of a live refresh, so it is paced separately
    // from the lists and adapts to what the machine can actually deliver. A
    // render that costs most of the frame budget means the map cannot sustain the
    // target rate, and pushing it only steals time from the scan for frames that
    // arrive late anyway.

    /// Target while a scan is running, when renders are comfortably fast.
    private static let fastInterval = 1.0 / 4.0
    /// Fallback when they are not.
    private static let slowInterval = 1.0
    /// A frame costing more than this fraction of the budget leaves no headroom;
    /// below it the rate is sustainable even though the renderer is busy. At 4 Hz
    /// this is 212 ms of a 250 ms window.
    private static let overBudget = 0.85
    /// Return to the fast rate only well below the budget, so the rate does not
    /// oscillate around the threshold.
    private static let comfortable = 0.65

    private var lastLiveRefresh: CFTimeInterval = 0
    /// Smoothed render cost in seconds; one slow frame should not flip the rate.
    private var averageRenderCost: Double = 0
    private(set) var liveInterval: Double = TreemapView.fastInterval
    private var pendingSize: CGSize = .zero
    private var trackingArea: NSTrackingArea?
    var metric: SizeMetric = .total {
        didSet { if metric != oldValue { requestRender() } }
    }
    /// Settings-derived options: logical sizes, package expansion, extra blocks.
    var renderOptions = TreemapOptions() {
        didSet { requestRender() }
    }
    /// Set from settings; when off, zooming swaps the map without animating.
    var animatesZoom = true
    /// Fires after each render completes, for diagnostics.
    var onRendered: ((TreemapImage, TreemapOptions) -> Void)?
    /// The most recent render, for diagnostics.
    var currentImage: TreemapImage? { current }
    /// Set when the chosen metric has nothing to show, so the view can say why
    /// rather than render an empty rectangle.
    private var emptyReason: String?
    /// Outlined in the map. Driven by the file list, so moving down the tree walks
    /// the highlight into ever smaller parts of the same rectangle instead of
    /// throwing the view away and starting again.
    var highlightedNode: Int32? {
        didSet { if highlightedNode != oldValue { needsDisplay = true } }
    }

    /// Zoom stack. Each entry remembers the rectangle the node occupied in its
    /// parent's map, which is what the zoom-out animation shrinks back into.
    private(set) var zoomStack: [(node: Int32, rectInParent: CGRect)] = [(0, .zero)]
    var zoomedNode: Int32 { zoomStack.last?.node ?? 0 }
    var canZoomOut: Bool { zoomStack.count > 1 }

    /// A zoom in progress: the outgoing image, the rectangle it expands from or
    /// collapses into, and where we are in the transition.
    private struct Transition {
        var outgoing: CGImage
        /// Stays nil until the new map has finished rendering. Drawing `current`
        /// before then would draw the outgoing image twice and swap mid-animation,
        /// which reads as a snap rather than a zoom.
        var incoming: CGImage?
        var focus: CGRect
        var zoomingIn: Bool
        var start: CFTimeInterval
    }
    private var transition: Transition?
    private var animationTimer: Timer?
    private static let zoomDuration: CFTimeInterval = 0.32

    override var isFlipped: Bool { true }

    func setArena(_ arena: NodeArena?) {
        self.arena = arena
        endTransition()
        zoomStack = [(0, .zero)]
        hoveredCell = -1
        current = nil
        requestRender()
    }

    func refresh() { requestRender() }

    /// Refresh driven by the scan's periodic tick, paced to what renders cost.
    func liveRefresh() {
        let now = CACurrentMediaTime()
        guard now - lastLiveRefresh >= liveInterval else { return }
        lastLiveRefresh = now
        requestRender()
    }

    /// Resets pacing at the start of a scan, so a slow previous tree does not
    /// hold the next one down.
    func resetLivePacing() {
        averageRenderCost = 0
        liveInterval = Self.fastInterval
        lastLiveRefresh = 0
    }

    private func recordRenderCost(_ seconds: Double) {
        // Weighted toward recent frames but not driven by any single one.
        averageRenderCost =
            averageRenderCost == 0 ? seconds : averageRenderCost * 0.7 + seconds * 0.3

        let previous = liveInterval
        if liveInterval == Self.fastInterval {
            if averageRenderCost > Self.fastInterval * Self.overBudget {
                liveInterval = Self.slowInterval
            }
        } else if averageRenderCost < Self.fastInterval * Self.comfortable {
            liveInterval = Self.fastInterval
        }
        if liveInterval != previous {
            DebugLog.line(
                String(
                    format: "map refresh -> %.1f Hz (frames averaging %.0f ms)",
                    1 / liveInterval, averageRenderCost * 1000))
        }
    }

    /// Returns to the top of the zoom stack immediately, without animating.
    ///
    /// Used when the displayed figure changes: wherever the user had drilled to is
    /// chosen for a different measure, and may be empty or meaningless under the
    /// new one.
    func resetZoom() {
        endTransition()
        guard zoomStack.count > 1 else { return }
        zoomStack = [(0, .zero)]
        hoveredCell = -1
        delegate?.treemapView(self, showTooltip: nil, at: .zero)
        delegate?.treemapView(self, didZoomTo: 0)
        requestRender()
    }

    func zoomOut() {
        guard zoomStack.count > 1 else { return }
        let leaving = zoomStack.removeLast()
        hoveredCell = -1
        beginTransition(focus: leaving.rectInParent, zoomingIn: false)
        requestRender()
        delegate?.treemapView(self, didZoomTo: zoomedNode)
    }

    /// Zooms to a node using its rectangle on the current map, so the animation
    /// starts from where that folder actually sits.
    func zoom(toNode node: Int32) {
        guard let current, let rect = current.directoryRects[node] else {
            zoom(to: node)
            return
        }
        let sx = bounds.width / Double(max(current.width, 1))
        let sy = bounds.height / Double(max(current.height, 1))
        zoom(
            to: node,
            from: CGRect(
                x: rect.minX * sx, y: rect.minY * sy,
                width: rect.width * sx, height: rect.height * sy))
    }

    func zoom(to node: Int32, from rect: CGRect? = nil) {
        guard let arena, arena.isDirectory(Int(node)), node != zoomedNode else { return }
        // Without a source rectangle (menu command rather than a click) fall back
        // to expanding from the centre.
        let focus = rect ?? CGRect(
            x: bounds.midX - bounds.width / 6, y: bounds.midY - bounds.height / 6,
            width: bounds.width / 3, height: bounds.height / 3)
        zoomStack.append((node, focus))
        hoveredCell = -1
        beginTransition(focus: focus, zoomingIn: true)
        requestRender()
        delegate?.treemapView(self, didZoomTo: node)
    }

    // MARK: - Zoom animation

    private func beginTransition(focus: CGRect, zoomingIn: Bool) {
        guard animatesZoom else { return }
        guard let image = current?.image, focus.width > 1, focus.height > 1 else { return }
        transition = Transition(
            outgoing: image, incoming: nil, focus: focus, zoomingIn: zoomingIn,
            start: CACurrentMediaTime())
        delegate?.treemapView(self, showTooltip: nil, at: .zero)
        animationTimer?.invalidate()
        let t = Timer(timeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.needsDisplay = true
            if CACurrentMediaTime() - (self.transition?.start ?? 0) >= Self.zoomDuration {
                self.endTransition()
            }
        }
        RunLoop.main.add(t, forMode: .common)
        animationTimer = t
    }

    private func endTransition() {
        animationTimer?.invalidate()
        animationTimer = nil
        transition = nil
        needsDisplay = true
    }

    /// Ease-in-out, so the motion starts and finishes gently.
    private func eased(_ t: Double) -> Double {
        t < 0.5 ? 2 * t * t : 1 - pow(-2 * t + 2, 2) / 2
    }

    /// Grows `focus` toward filling the view.
    ///
    /// The scale is deliberately uniform. Scaling x and y independently makes a
    /// wide block stretch sideways rather than zoom, which is what made the
    /// transition look like the map lengthening. Uniform scale over-fills the
    /// short axis instead, which is what a zoom actually looks like.
    private func expandTransform(_ focus: CGRect, progress: Double) -> CGAffineTransform {
        guard focus.width > 0.5, focus.height > 0.5 else { return .identity }
        // A block that already fills most of the view yields a scale near 1, so the
        // transition degenerates into a plain cross-fade — "it faded but didn't
        // zoom". Enforce a floor so the motion always reads as a zoom.
        let minimumZoom = 1.45
        let full = max(
            bounds.width / focus.width, bounds.height / focus.height, minimumZoom)
        let scale = 1 + (full - 1) * progress
        let centre = CGPoint(x: focus.midX, y: focus.midY)
        // The focus centre travels to the view centre as the scale grows.
        let targetX = centre.x + (bounds.midX - centre.x) * progress
        let targetY = centre.y + (bounds.midY - centre.y) * progress
        return CGAffineTransform(
            translationX: targetX - centre.x * scale, y: targetY - centre.y * scale
        ).scaledBy(x: scale, y: scale)
    }

    /// The reverse: shrinks the whole view down into `focus`.
    private func shrinkTransform(_ focus: CGRect, progress: Double) -> CGAffineTransform {
        expandTransform(focus, progress: progress).inverted()
    }

    // MARK: - Rendering

    private func requestRender() {
        guard let arena, arena.count > 0, bounds.width > 1, bounds.height > 1 else { return }
        emptyReason = arena.size(metric, Int(zoomedNode)) > 0 ? nil : "Nothing matches \(metric.title)"
        let scale = window?.backingScaleFactor ?? 2
        let w = Int(bounds.width * scale)
        let h = Int(bounds.height * scale)
        guard w > 0, h > 0 else { return }

        // One render at a time. A render takes ~200 ms on a large tree, so at
        // refresh rates above ~4 Hz requests arrive faster than they complete —
        // without this, each one starts work that the next immediately discards,
        // burning a core for nothing.
        if isRendering {
            renderQueued = true
            return
        }
        isRendering = true
        renderGeneration &+= 1
        let generation = renderGeneration
        let root = zoomedNode
        let palette = self.palette
        var options = renderOptions
        options.metric = metric
        options.pixelScale = Double(scale)

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let began = DispatchTime.now().uptimeNanoseconds
            let rendered = Treemap.render(
                arena: arena, root: root, width: w, height: h, palette: palette,
                options: options)
            let elapsed = Double(DispatchTime.now().uptimeNanoseconds - began) / 1e6
            DebugLog.line(
                String(
                    format:
                        "render %dx%d scale %.1f root %d metric %@ logical %@ packages %@ -> %d cells in %.1f ms",
                    w, h, options.pixelScale, Int(root), options.metric.title as NSString,
                    options.useLogicalSize ? "yes" : "no",
                    options.expandPackages ? "yes" : "no", rendered.cells.count, elapsed))
            DispatchQueue.main.async {
                guard let self else { return }
                self.isRendering = false
                self.recordRenderCost(elapsed / 1000)
                let queued = self.renderQueued
                self.renderQueued = false
                defer { if queued { self.requestRender() } }
                guard generation == self.renderGeneration else { return }
                self.current = rendered
                self.onRendered?(rendered, options)
                // Hand the finished map to any zoom in flight, so it starts
                // cross-fading only once there is something new to fade to.
                if self.transition != nil { self.transition?.incoming = rendered.image }
                self.needsDisplay = true
            }
        }
    }

    override func layout() {
        super.layout()
        if bounds.size != pendingSize {
            pendingSize = bounds.size
            requestRender()
        }
        updateTracking()
    }

    private func updateTracking() {
        if let t = trackingArea { removeTrackingArea(t) }
        let t = NSTrackingArea(
            rect: bounds, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow],
            owner: self, userInfo: nil)
        addTrackingArea(t)
        trackingArea = t
    }

    /// Draws a CGImage the right way up in this flipped view.
    ///
    /// The view is `isFlipped`, so y grows downward and matches the image's own row
    /// order — but CoreGraphics still draws images bottom-up, which mirrors them
    /// vertically. Every overlay (selection, hover, hit testing) works in unflipped
    /// image coordinates, so without this the content and everything drawn on top
    /// of it sit in different coordinate systems.
    private func drawImage(_ image: CGImage, in rect: CGRect, into ctx: CGContext) {
        ctx.saveGState()
        ctx.translateBy(x: 0, y: rect.origin.y * 2 + rect.height)
        ctx.scaleBy(x: 1, y: -1)
        ctx.draw(image, in: rect)
        ctx.restoreGState()
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.setFillColor(NSColor(srgbRed: 0.11, green: 0.11, blue: 0.12, alpha: 1).cgColor)
        ctx.fill(bounds)

        if let emptyReason {
            drawCentredMessage(emptyReason, in: ctx)
            return
        }
        if let transition {
            let raw = min(1, max(0, (CACurrentMediaTime() - transition.start) / Self.zoomDuration))
            let p = eased(raw)
            let focus = transition.focus

            // Zoom in:  old map grows until the clicked block fills the view,
            //           new map starts shrunk into that block and settles.
            // Zoom out: exactly the reverse, so the two directions mirror.
            let outgoing =
                transition.zoomingIn
                ? expandTransform(focus, progress: p)
                : shrinkTransform(focus, progress: p)
            let incomingTransform =
                transition.zoomingIn
                ? shrinkTransform(focus, progress: 1 - p)
                : expandTransform(focus, progress: 1 - p)

            // Incoming underneath, outgoing fading out on top.
            if let incoming = transition.incoming {
                ctx.saveGState()
                ctx.concatenate(incomingTransform)
                ctx.setAlpha(p)
                drawImage(incoming, in: bounds, into: ctx)
                ctx.restoreGState()
            }

            ctx.saveGState()
            ctx.concatenate(outgoing)
            // Hold the outgoing map fully opaque until the new one exists, so a
            // slow render never shows a half-empty frame.
            ctx.setAlpha(transition.incoming == nil ? 1 : 1 - p)
            drawImage(transition.outgoing, in: bounds, into: ctx)
            ctx.restoreGState()
            return
        }

        guard let current, let image = current.image else {
            drawCentredMessage("Scanning…", in: ctx)
            return
        }
        drawImage(image, in: bounds, into: ctx)

        let sx = bounds.width / Double(max(current.width, 1))
        let sy = bounds.height / Double(max(current.height, 1))
        func viewRect(_ rect: CGRect) -> CGRect {
            CGRect(
                x: rect.minX * sx, y: rect.minY * sy,
                width: rect.width * sx, height: rect.height * sy)
        }

        // Same-coloured neighbours blend into what looks like one rectangle, so
        // show the extent of whatever the pointer is on.
        if hoveredCell >= 0, hoveredCell < current.cells.count {
            let outline = viewRect(current.cells[hoveredCell].rect).insetBy(dx: 1, dy: 1)
            if outline.width > 2, outline.height > 2 {
                ctx.setStrokeColor(NSColor.black.withAlphaComponent(0.45).cgColor)
                ctx.setLineWidth(3)
                ctx.stroke(outline)
                ctx.setStrokeColor(NSColor.white.withAlphaComponent(0.85).cgColor)
                ctx.setLineWidth(1.5)
                ctx.stroke(outline)
            }
        }

        // The current selection, in a colour the palette never produces so it
        // cannot be mistaken for content.
        if let node = highlightedNode, let rect = current.directoryRects[node] {
            // Normally stroked exactly on the rectangle's edge, not inset.
            //
            // Adjacent folders share an edge exactly, so any inset at all puts the
            // outlines of two neighbours a gap apart — which reads as a strip of
            // unclaimed space even though their rectangles touch. On the edge, two
            // neighbours' outlines land in the same place.
            //
            // A small rectangle is the exception. An outline on the edge of a
            // 17x12pt cell eats roughly 3pt a side, leaving almost none of the
            // cell's own colour visible — so the selection reads as whatever its
            // neighbours are coloured. Below that size the outline goes outside the
            // rectangle instead, leaving the cell itself untouched.
            let base = viewRect(rect)
            let isSmall = base.width < 26 || base.height < 26
            let outline = isSmall ? base.insetBy(dx: -2.5, dy: -2.5) : base
            if outline.width > 2, outline.height > 2 {
                ctx.setStrokeColor(NSColor.black.withAlphaComponent(0.55).cgColor)
                ctx.setLineWidth(isSmall ? 2 : 3)
                ctx.stroke(outline)
                ctx.setStrokeColor(NSColor.systemYellow.cgColor)
                ctx.setLineWidth(1.5)
                ctx.stroke(outline)
            }
        }
    }

    private func drawCentredMessage(_ text: String, in ctx: CGContext) {
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 13),
            .foregroundColor: NSColor.secondaryLabelColor,
        ]
        let s = NSAttributedString(string: text, attributes: attrs)
        let size = s.size()
        s.draw(at: NSPoint(x: bounds.midX - size.width / 2, y: bounds.midY - size.height / 2))
    }

    /// The two lines to show for whatever is under the pointer, or nil if nothing is.
    func tooltipContent() -> (title: String, detail: String)? {
        guard let arena, let current, hoveredCell >= 0, hoveredCell < current.cells.count,
            arena.count > 0
        else { return nil }
        let cell = current.cells[hoveredCell]
        // Free / other space are not nodes in the tree.
        if let block = current.extraBlocks[cell.node] {
            return (block.label, Format.bytes(block.bytes))
        }
        let node = Int(node(forCellAt: hoveredCell))
        guard node >= 0, node < arena.count else { return nil }

        // Relative to what is on screen, so the label identifies the square rather
        // than repeating the whole absolute path on every hover.
        let rootPath = arena.path(Int(zoomedNode))
        let fullPath = arena.path(node)
        var title = fullPath
        if fullPath.hasPrefix(rootPath), fullPath.count > rootPath.count {
            title = String(fullPath.dropFirst(rootPath.count).drop(while: { $0 == "/" }))
        }
        if title.isEmpty { title = arena.name(node) }

        let detail: String
        if cell.aggregatedCount > 0 {
            detail = "\(Format.count(Int(cell.aggregatedCount))) files too small to draw separately"
        } else {
            var parts = [Format.bytes(arena.size(metric, node))]
            if let kind = describedKind(of: node) { parts.append(kind) }
            let name = arena.name(node)
            if let dot = name.lastIndex(of: "."), dot != name.startIndex {
                let ext = name[name.index(after: dot)...]
                if !ext.isEmpty, ext.count <= 12 { parts.append(".\(ext)") }
            }
            if arena.isDirectory(node) {
                parts.append("\(Format.count(Int(arena.fileCount[node]))) files")
            }
            detail = parts.joined(separator: " · ")
        }
        return (title, detail)
    }

    /// The kind to name in a tooltip, or nil for a plain folder.
    ///
    /// Directories carry the folder kind, which says nothing useful. A packaged
    /// directory — a disk image, bundle or archive that the map draws as a single
    /// block — is better described by what its extension says it is.
    private func describedKind(of node: Int) -> String? {
        guard let arena else { return nil }
        var kind = arena.kind[node]
        if kind == FileKind.directory {
            let bytes = arena.nameBytes(node)
            guard let base = bytes.baseAddress, bytes.count > 0 else { return nil }
            kind = FileKind.classify(name: UnsafeRawPointer(base), length: bytes.count)
        }
        let index = Int(kind)
        guard index > 0, index < FileKind.names.count else { return nil }
        // Nothing to say for a folder with no meaningful extension.
        if kind == FileKind.directory || kind == FileKind.noExtension { return nil }
        return FileKind.names[index]
    }

    /// Hands the tooltip to the window, which draws it above the other panes. Drawn
    /// inside this view it was clipped by the pane edge.
    private func publishTooltip() {
        guard let content = tooltipContent() else {
            delegate?.treemapView(self, showTooltip: nil, at: .zero)
            return
        }
        let inWindow = convert(mouseLocation, to: nil)
        delegate?.treemapView(self, showTooltip: content, at: inWindow)
    }

    /// The node a rectangle stands for.
    ///
    /// Every square is one file, so this is simply the cell's own node. A pooled
    /// cell is the exception: its node is only a representative of the many
    /// sub-pixel items it covers, so its folder is the honest answer.
    private func node(forCellAt index: Int) -> Int32 {
        guard let arena, let current, index >= 0, index < current.cells.count else {
            return zoomedNode
        }
        let cell = current.cells[index]
        let hit = Int(cell.node)
        guard cell.node >= 0, hit < arena.count else { return cell.node }
        if cell.aggregatedCount > 0 {
            return arena.isDirectory(hit) ? Int32(hit) : arena.parent[hit]
        }
        return cell.node
    }

    /// The rectangle a folder occupies, for the selection outline and for zooming.
    private func blockRect(of node: Int32) -> CGRect? {
        current?.directoryRects[node]
    }


    // MARK: - Hit testing

    private var mouseLocation: CGPoint = .zero

    /// One array lookup, independent of how many rectangles are on screen.
    private func cellIndex(at point: CGPoint) -> Int {
        guard let current, current.width > 0 else { return -1 }
        let sx = Double(current.width) / Double(bounds.width)
        let sy = Double(current.height) / Double(bounds.height)
        return Int(current.cellIndex(atX: Int(point.x * sx), y: Int(point.y * sy)))
    }

    override func mouseMoved(with event: NSEvent) {
        mouseLocation = convert(event.locationInWindow, from: nil)
        let hit = cellIndex(at: mouseLocation)
        if hit != hoveredCell {
            hoveredCell = hit
            needsDisplay = true
        }
        publishTooltip()
    }

    override func mouseExited(with event: NSEvent) {
        hoveredCell = -1
        needsDisplay = true
        delegate?.treemapView(self, showTooltip: nil, at: .zero)
    }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        let hit = cellIndex(at: p)
        guard let current, hit >= 0, hit < current.cells.count else { return }
        guard let arena else { return }
        let named = node(forCellAt: hit)
        // Clicking free / other space selects nothing: it is not in the tree.
        guard named >= 0 else { return }

        // A single click selects and leaves the map alone: keeping one stable
        // picture while the selection moves through it is what lets the user hold
        // their bearings. Double-click is the explicit "go inside".
        if event.clickCount >= 2 {
            let target = arena.isDirectory(Int(named)) ? named : arena.parent[Int(named)]
            guard target != zoomedNode, arena.isDirectory(Int(target)) else { return }
            let sx = bounds.width / Double(max(current.width, 1))
            let sy = bounds.height / Double(max(current.height, 1))
            let r = blockRect(of: target) ?? current.cells[hit].rect
            zoom(
                to: target,
                from: CGRect(
                    x: r.minX * sx, y: r.minY * sy, width: r.width * sx, height: r.height * sy))
            return
        }

        highlightedNode = named
        delegate?.treemapView(self, didSelect: named)
    }
}
