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
    private var pendingSize: CGSize = .zero
    private var trackingArea: NSTrackingArea?
    var metric: SizeMetric = .total {
        didSet { if metric != oldValue { requestRender() } }
    }
    /// Set when the chosen metric has nothing to show, so the view can say why
    /// rather than render an empty rectangle.
    private var emptyReason: String?

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

        renderGeneration &+= 1
        let generation = renderGeneration
        let root = zoomedNode
        let palette = self.palette
        let metric = self.metric

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let rendered = Treemap.render(
                arena: arena, root: root, width: w, height: h, palette: palette, metric: metric)
            DispatchQueue.main.async {
                guard let self, generation == self.renderGeneration else { return }
                self.current = rendered
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
                ctx.draw(incoming, in: bounds)
                ctx.restoreGState()
            }

            ctx.saveGState()
            ctx.concatenate(outgoing)
            // Hold the outgoing map fully opaque until the new one exists, so a
            // slow render never shows a half-empty frame.
            ctx.setAlpha(transition.incoming == nil ? 1 : 1 - p)
            ctx.draw(transition.outgoing, in: bounds)
            ctx.restoreGState()
            return
        }

        guard let current, let image = current.image else {
            drawCentredMessage("Scanning…", in: ctx)
            return
        }
        ctx.draw(image, in: bounds)

        // Same-coloured neighbours blend into what looks like one rectangle, so
        // show the actual extent of whatever the tooltip is describing.
        if hoveredCell >= 0, let rect = blockRect(forCellAt: hoveredCell) {
            let sx = bounds.width / Double(max(current.width, 1))
            let sy = bounds.height / Double(max(current.height, 1))
            let outline = CGRect(
                x: rect.minX * sx, y: rect.minY * sy,
                width: rect.width * sx, height: rect.height * sy
            ).insetBy(dx: 1, dy: 1)
            guard outline.width > 2, outline.height > 2 else { return }
            ctx.setStrokeColor(NSColor.black.withAlphaComponent(0.55).cgColor)
            ctx.setLineWidth(3)
            ctx.stroke(outline)
            ctx.setStrokeColor(NSColor.white.withAlphaComponent(0.95).cgColor)
            ctx.setLineWidth(1.5)
            ctx.stroke(outline)
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
        let node = Int(blockNode(forCellAt: hoveredCell))
        guard node < arena.count else { return nil }
        let pooled = current.cells[hoveredCell].aggregatedCount

        let size = Format.bytes(arena.size(metric, node))
        // Relative to what is on screen, so the label identifies the block instead
        // of repeating the whole absolute path on every hover.
        let rootPath = arena.path(Int(zoomedNode))
        let fullPath = arena.path(node)
        var title = fullPath
        if fullPath.hasPrefix(rootPath), fullPath.count > rootPath.count {
            title = String(fullPath.dropFirst(rootPath.count).drop(while: { $0 == "/" }))
        }
        if title.isEmpty { title = arena.name(node) }

        let detail: String
        if arena.isDirectory(node) {
            var text = "\(size) · \(Format.count(Int(arena.fileCount[node]))) files"
            if pooled > 0 {
                text += " · \(Format.count(Int(pooled))) too small to draw separately"
            }
            detail = text
        } else {
            detail = "\(size) · \(FileKind.names[Int(arena.kind[node])])"
        }
        return (title, detail)
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

    /// Smallest on-screen rectangle worth treating as its own block, in device
    /// pixels. Below this a folder is too small to point at meaningfully, so its
    /// parent is named instead.
    private static let minimumBlockArea: CGFloat = 22 * 22

    /// What the pointer is actually on: a big enough rectangle to be its own
    /// block, whether that is a single large file or a folder.
    ///
    /// A 10 GB disk image drawn as one large rectangle should be named as that
    /// file — reporting its enclosing folder instead describes something the user
    /// cannot see and cannot act on. Only when the rectangle under the pointer is
    /// too small to point at does this climb to a grouping.
    ///
    /// Hovering and clicking both use this, so the tooltip always names exactly
    /// what a click will act on.
    private func blockNode(forCellAt index: Int) -> Int32 {
        guard let arena, let current, index >= 0, index < current.cells.count else {
            return zoomedNode
        }
        let cell = current.cells[index]
        let hit = Int(cell.node)
        guard hit < arena.count else { return zoomedNode }
        let root = Int(zoomedNode)

        // A directory under the pointer is the block itself.
        if arena.isDirectory(hit), hit != root,
            let rect = current.directoryRects[Int32(hit)],
            rect.width * rect.height >= Self.minimumBlockArea
        {
            return outermostMatchingBlock(Int32(hit))
        }

        var node = arena.isDirectory(hit) ? hit : Int(arena.parent[hit])
        var hops = 0
        while node != root, node > 0, hops < 1024 {
            if let rect = current.directoryRects[Int32(node)],
                rect.width * rect.height >= Self.minimumBlockArea
            {
                let folder = outermostMatchingBlock(Int32(node))
                // Name the file only when it effectively *is* the folder's block —
                // a 10 GB image filling its bundle. Otherwise the folder is the
                // answer, so that hovering anywhere inside it gives one stable
                // result instead of changing from file to file.
                if cell.aggregatedCount == 0, !arena.isDirectory(hit),
                    let folderRect = current.directoryRects[folder]
                {
                    let cellArea = cell.rect.width * cell.rect.height
                    let folderArea = folderRect.width * folderRect.height
                    if folderArea > 0, cellArea >= folderArea * 0.6 { return Int32(hit) }
                }
                return folder
            }
            let parent = Int(arena.parent[node])
            if parent == node { break }
            node = parent
            hops += 1
        }
        return zoomedNode
    }

    /// The rectangle covered by the block the pointer resolves to, in image pixels.
    private func blockRect(forCellAt index: Int) -> CGRect? {
        guard let arena, let current, index >= 0, index < current.cells.count else { return nil }
        let node = blockNode(forCellAt: index)
        if arena.isDirectory(Int(node)) { return current.directoryRects[node] }
        return current.cells[index].rect
    }

    /// Climbs through ancestors that occupy essentially the same rectangle.
    ///
    /// Nesting like `system-images/android-36.1/google_apis/arm64-v8a` draws four
    /// folders at identical size, because each has a single dominant child. They
    /// are one block to the eye, and the outermost is the one worth naming.
    private func outermostMatchingBlock(_ node: Int32) -> Int32 {
        guard let arena, let current,
            var area = current.directoryRects[node].map({ $0.width * $0.height })
        else { return node }
        let root = Int(zoomedNode)
        var best = Int(node)
        var hops = 0
        while best != root, best > 0, hops < 64 {
            let parent = Int(arena.parent[best])
            if parent == best || parent == root { break }
            guard let rect = current.directoryRects[Int32(parent)] else { break }
            let parentArea = rect.width * rect.height
            // A parent barely bigger than its child adds nothing visually.
            guard parentArea <= area * 1.15 else { break }
            best = parent
            area = parentArea
            hops += 1
        }
        return Int32(best)
    }

    /// The rectangle a block occupies, preferring the container cell over any leaf
    /// drawn inside it.
    private func blockRect(of node: Int32) -> CGRect? {
        guard let current else { return nil }
        var fallback: CGRect?
        for cell in current.cells where cell.node == node {
            if !cell.isLeaf { return cell.rect }
            if fallback == nil { fallback = cell.rect }
        }
        return fallback
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
        // The same resolution the tooltip used, so clicking zooms into precisely
        // the folder that was named under the pointer.
        let named = blockNode(forCellAt: hit)
        delegate?.treemapView(self, didSelect: named)
        // A file cannot be zoomed into, so a click on one descends to its folder.
        let target = arena.isDirectory(Int(named)) ? named : arena.parent[Int(named)]
        guard target != zoomedNode, arena.isDirectory(Int(target)) else { return }

        // Rectangles are in image pixels; the animation works in view points.
        let sx = bounds.width / Double(max(current.width, 1))
        let sy = bounds.height / Double(max(current.height, 1))
        let r = blockRect(of: target) ?? current.cells[hit].rect
        zoom(
            to: target,
            from: CGRect(
                x: r.minX * sx, y: r.minY * sy, width: r.width * sx, height: r.height * sy))
    }
}
