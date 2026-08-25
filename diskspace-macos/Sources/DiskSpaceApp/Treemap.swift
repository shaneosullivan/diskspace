import AppKit
import DiskScanCore

/// Accumulated cushion surface: the quadratic height field z(x,y) built up by one
/// ridge per hierarchy level, per van Wijk & van de Wetering.
struct Cushion {
    var a = 0.0  // x² coefficient
    var b = 0.0  // y² coefficient
    var c = 0.0  // x coefficient
    var d = 0.0  // y coefficient

    /// Adds this rectangle's ridge to the surface. Called once per level as the
    /// recursion descends, so a leaf carries the sum of every enclosing folder's
    /// ridge — which is what makes directory structure visible without borders.
    mutating func addRidge(_ rect: CGRect, height h: Double) {
        guard rect.width > 0, rect.height > 0 else { return }
        c += 4 * h * (rect.maxX + rect.minX) / rect.width
        a -= 4 * h / rect.width
        d += 4 * h * (rect.maxY + rect.minY) / rect.height
        b -= 4 * h / rect.height
    }
}

struct TreemapCell {
    var rect: CGRect
    var node: Int32
    var depth: Int
    var isLeaf: Bool
    /// >0 when this cell stands in for that many items too small to draw
    /// individually. They are pooled rather than dropped, so no gap appears.
    var aggregatedCount: Int32 = 0
    /// Why a directory ended up drawn as a solid block instead of recursed into.
    var leafReason: LeafReason = .file
    /// Surface used to shade this cell, accumulated from every enclosing folder.
    var cushion = Cushion()
}

enum LeafReason: String {
    case file
    case noChildren
    case depthLimit
    case rectTooSmall
    case noDrawableItems
    case pooled
}

/// A rendered treemap: the pixels to blit, plus a parallel buffer holding the cell
/// index at every pixel.
///
/// The ID buffer is what makes hover instant. Hit testing is `ids[y * width + x]`
/// — one array read, no traversal, no matter how many rectangles are on screen.
/// Both buffers are produced together on a background thread.
final class TreemapImage {
    let width: Int
    let height: Int
    let cells: [TreemapCell]
    let ids: [Int32]
    let image: CGImage?
    /// The node this map is rooted at, for zoom bookkeeping.
    let rootNode: Int32
    /// Rectangle drawn for each node — folders and files alike. Used both to ask
    /// how big a block really is on screen and to highlight whatever is selected
    /// in the file list.
    let directoryRects: [Int32: CGRect]
    /// Free / other space blocks, keyed by their negative sentinel id.
    let extraBlocks: [Int32: ExtraBlock]

    init(
        width: Int, height: Int, cells: [TreemapCell], ids: [Int32], image: CGImage?,
        rootNode: Int32, directoryRects: [Int32: CGRect] = [:],
        extraBlocks: [Int32: ExtraBlock] = [:]
    ) {
        self.width = width
        self.height = height
        self.cells = cells
        self.ids = ids
        self.image = image
        self.rootNode = rootNode
        self.directoryRects = directoryRects
        self.extraBlocks = extraBlocks
    }

    func cellIndex(atX x: Int, y: Int) -> Int32 {
        guard x >= 0, y >= 0, x < width, y < height else { return -1 }
        return ids[y * width + x]
    }
}

/// A block drawn alongside the scanned folder's contents: free space on the
/// drive, or everything else in use on it.
struct ExtraBlock {
    var id: Int32
    var label: String
    var bytes: Int64
    var colour: UInt32
}

struct TreemapOptions {
    var metric: SizeMetric = .total
    /// Show the length of file contents rather than the space occupied on disk.
    var useLogicalSize = false
    /// Descend into bundles instead of drawing each as a single block.
    var expandPackages = false
    var pixelScale: Double = 1
    var extraBlocks: [ExtraBlock] = []
}

enum Treemap {
    /// Rectangles below this size are not worth drawing or recursing into. The
    /// original app rendered far past the point of visual distinguishability,
    /// which is a large amount of work for pixels nobody can tell apart.
    static let minimumSide: Double = 3
    /// Only a stack-overflow backstop. The real stopping rule is `minimumSide`:
    /// stop when a rectangle is too small to see. A tight depth limit instead made
    /// deep trees bottom out early and render whole directories as grey blocks.
    static let maximumDepth = 64
    /// Every file gets its own rectangle down to this area. Only genuinely
    /// sub-pixel items are pooled, and only so that no gap is left behind.
    static let minimumCellArea: Double = 1

    // Cushion shading parameters.
    /// Ridge height at the top level.
    static let ridgeHeight = 0.38
    /// Each level's ridge is this much shallower than its parent's, so the
    /// hierarchy reads as nested bumps rather than uniform noise.
    /// How much shallower each level's ridge is than its parent's.
    ///
    /// This number decides whether the map reads as a hierarchy at all. At 0.7 a
    /// leaf eight levels down keeps 0.7^8 ≈ 6% of the base ridge — effectively
    /// flat, so whole folders melted into one smooth gradient and a folder's
    /// rectangle could cut straight through what looked like a single block. At
    /// 0.91 the same leaf keeps ≈ 47%, so every file still reads as its own
    /// cushion inside its folder's cushion.
    static let ridgeScaleFactor = 0.91
    /// Slight lift so cushion edges do not bottom out to black.
    static let renderGain = 1.04
    static let ambientLight = 0.20
    /// Light direction, from the upper left and close to head-on.
    ///
    /// A steeply angled light (-8, -8, 10) drove `cosa` to zero across most of
    /// every cushion, so large areas fell into flat shadow and lost their shape.
    /// Keeping the light near the viewer preserves the dome on every block.
    static let lightXY = -2.5
    static let lightLen = (lightXY * lightXY * 2 + 100).squareRoot()
    static let lightX = lightXY / lightLen
    static let lightY = lightXY / lightLen
    static let lightZ = 10.0 / lightLen

    // MARK: - Squarified layout

    /// Classic squarified treemap: pack rows so cell aspect ratios stay near 1.
    static func layout(
        _ items: [(node: Int32, value: Double)], in bounds: CGRect, preserveOrder: Bool = false
    ) -> [(node: Int32, rect: CGRect)] {
        var out: [(Int32, CGRect)] = []
        var items = items.filter { $0.value > 0 }
        guard !items.isEmpty, bounds.width > 0, bounds.height > 0 else { return [] }
        if !preserveOrder { items.sort { $0.value > $1.value } }

        var total = items.reduce(0.0) { $0 + $1.value }
        var rect = bounds
        var i = 0

        while i < items.count, total > 0, rect.width > 0, rect.height > 0 {
            let shortSide = min(rect.width, rect.height)
            let scale = (rect.width * rect.height) / total

            var rowSum = 0.0
            var rowEnd = i
            var bestRatio = Double.infinity
            while rowEnd < items.count {
                let candidateSum = rowSum + items[rowEnd].value
                let ratio = worstAspect(
                    items[i...rowEnd].map(\.value), sum: candidateSum,
                    shortSide: shortSide, scale: scale)
                if ratio > bestRatio { break }
                bestRatio = ratio
                rowSum = candidateSum
                rowEnd += 1
            }
            if rowEnd == i { rowEnd = i + 1; rowSum = items[i].value }

            let thickness = (rowSum * scale) / shortSide
            var offset = 0.0
            let horizontal = rect.width >= rect.height
            for j in i..<rowEnd {
                let cellLength = (items[j].value * scale) / max(thickness, 1e-9)
                let r: CGRect =
                    horizontal
                    ? CGRect(x: rect.minX, y: rect.minY + offset, width: thickness, height: cellLength)
                    : CGRect(x: rect.minX + offset, y: rect.minY, width: cellLength, height: thickness)
                out.append((items[j].node, r))
                offset += cellLength
            }

            if horizontal {
                rect = CGRect(
                    x: rect.minX + thickness, y: rect.minY,
                    width: rect.width - thickness, height: rect.height)
            } else {
                rect = CGRect(
                    x: rect.minX, y: rect.minY + thickness,
                    width: rect.width, height: rect.height - thickness)
            }
            total -= rowSum
            i = rowEnd
        }
        return out
    }

    private static func worstAspect(
        _ values: [Double], sum: Double, shortSide: Double, scale: Double
    ) -> Double {
        let thickness = (sum * scale) / shortSide
        guard thickness > 0 else { return .infinity }
        var worst = 0.0
        for v in values {
            let length = (v * scale) / thickness
            guard length > 0 else { return .infinity }
            worst = max(worst, max(thickness / length, length / thickness))
        }
        return worst
    }

    // MARK: - Recursive build

    /// Builds the full cell list, stopping at rectangles too small to see.
    /// The figure to draw a node at, honouring the physical/logical setting.
    @inline(__always)
    static func size(_ arena: NodeArena, _ node: Int, _ options: TreemapOptions) -> Int64 {
        // Only the total has both forms; the recency figures are accumulated from
        // occupied space alone.
        if options.useLogicalSize, options.metric == .total { return arena.logical[node] }
        return arena.size(options.metric, node)
    }

    /// Whether a directory should be drawn as one block rather than opened up.
    @inline(__always)
    static func isSealedPackage(_ arena: NodeArena, _ node: Int, _ options: TreemapOptions)
        -> Bool
    {
        guard !options.expandPackages, arena.isDirectory(node) else { return false }
        let bytes = arena.nameBytes(node)
        guard let base = bytes.baseAddress, bytes.count > 0 else { return false }
        return FileKind.isPackage(name: UnsafeRawPointer(base), length: bytes.count)
    }

    static func build(
        arena: NodeArena, root: Int32, bounds: CGRect, options: TreemapOptions
    ) -> [TreemapCell] {
        var cells: [TreemapCell] = []
        cells.reserveCapacity(4096)
        recurse(
            arena: arena, node: root, rect: bounds, depth: 0, options: options,
            cushion: Cushion(), ridge: ridgeHeight, into: &cells)
        return cells
    }

    private static func recurse(
        arena: NodeArena, node: Int32, rect: CGRect, depth: Int, options: TreemapOptions,
        cushion inherited: Cushion, ridge: Double, into cells: inout [TreemapCell]
    ) {
        // Every level contributes its own ridge on top of its ancestors'.
        var cushion = inherited
        if depth > 0 { cushion.addRidge(rect, height: ridge) }
        let childRidge = ridge * ridgeScaleFactor
        let extras = depth == 0 ? options.extraBlocks : []
        let i = Int(node)
        // A sealed package is one item, so it never opens up.
        let children = isSealedPackage(arena, i, options) ? 0..<0 : arena.children(of: i)
        // Area, not side length. A 2x500 sliver is 1000 visible pixels; bailing on
        // it painted a large folder-coloured block where real content should be.
        let tooSmall =
            rect.width < 1 || rect.height < 1
            || rect.width * rect.height < minimumSide * minimumSide

        if children.isEmpty || depth >= maximumDepth || tooSmall {
            let reason: LeafReason =
                children.isEmpty ? .noChildren : (depth >= maximumDepth ? .depthLimit : .rectTooSmall)
            cells.append(
                TreemapCell(
                    rect: rect, node: node, depth: depth, isLeaf: true, leafReason: reason,
                    cushion: cushion))
            return
        }

        var items: [(node: Int32, value: Double)] = []
        items.reserveCapacity(children.count + extras.count)
        for block in extras where block.bytes > 0 {
            items.append((block.id, Double(block.bytes)))
        }
        for c in children {
            let v = Double(size(arena, c, options))
            if v > 0 { items.append((Int32(c), v)) }
        }
        guard !items.isEmpty else {
            cells.append(
                TreemapCell(
                    rect: rect, node: node, depth: depth, isLeaf: true,
                    leafReason: .noDrawableItems))
            return
        }

        // No inset. Blocks are told apart by their cushion shading rather than by
        // gaps or outlines, so the map tiles completely and no background shows.
        let inner = rect
        guard inner.width >= 1, inner.height >= 1,
            inner.width * inner.height >= minimumSide * minimumSide
        else {
            cells.append(
                TreemapCell(
                    rect: rect, node: node, depth: depth, isLeaf: true,
                    leafReason: .rectTooSmall, cushion: cushion))
            return
        }

        // Pool the items too small to render into one cell, instead of dropping
        // them. Dropping left their area showing the background through as noise.
        let totalValue = items.reduce(0.0) { $0 + $1.value }
        let scale = (inner.width * inner.height) / max(totalValue, 1)
        var drawable: [(node: Int32, value: Double)] = []
        var pooledValue = 0.0
        var pooledCount: Int32 = 0
        var pooledKind: Int32 = -1
        for item in items {
            if item.value * scale >= minimumCellArea {
                drawable.append(item)
            } else {
                if pooledKind < 0 { pooledKind = item.node }  // items are size-sorted
                pooledValue += item.value
                pooledCount += 1
            }
        }
        // Tagged with a sentinel rather than identified by position: `layout`
        // re-sorts, so index or value matching afterwards would be unreliable.
        // Order the children so that files of the same kind sit together.
        //
        // Laid out purely by size, files of one kind end up scattered and a folder
        // boundary can fall in the middle of an apparently uniform area. Grouping
        // by kind makes a folder's contents read as contiguous bands of colour, so
        // its edge lands where the colour changes. Groups are still ordered largest
        // first, which is what keeps the squarified aspect ratios reasonable.
        var groupTotals: [Int32: Double] = [:]
        var groupItems: [Int32: [(node: Int32, value: Double)]] = [:]
        for item in drawable {
            let index = max(0, Int(item.node))
            // Each directory is its own group: it is a nested map, not a colour.
            let key: Int32
            if item.node < 0 {
                key = item.node  // free / other space: its own group
            } else {
                key = arena.isDirectory(index) ? -(item.node + 1) : Int32(arena.kind[index])
            }
            groupTotals[key, default: 0] += item.value
            groupItems[key, default: []].append(item)
        }
        var ordered: [(node: Int32, value: Double)] = []
        ordered.reserveCapacity(drawable.count)
        for key in groupTotals.keys.sorted(by: { groupTotals[$0]! > groupTotals[$1]! }) {
            ordered.append(contentsOf: groupItems[key]!.sorted { $0.value > $1.value })
        }
        drawable = ordered

        let pooledSentinel = Int32.min
        if pooledCount > 0, pooledValue * scale >= 1 {
            drawable.append((node: pooledSentinel, value: pooledValue))
        }
        guard !drawable.isEmpty else {
            cells.append(
                TreemapCell(
                    rect: rect, node: node, depth: depth, isLeaf: true,
                    leafReason: .noDrawableItems, cushion: cushion))
            return
        }

        cells.append(
            TreemapCell(
                rect: rect, node: node, depth: depth, isLeaf: false, cushion: cushion))
        for (child, childRect) in layout(drawable, in: inner, preserveOrder: true) {
            if child == pooledSentinel {
                // Coloured by the largest item it stands for, so the pooled block
                // still reads as belonging to whatever dominates there.
                var leafCushion = cushion
                leafCushion.addRidge(childRect, height: childRidge)
                cells.append(
                    TreemapCell(
                        rect: childRect, node: pooledKind, depth: depth + 1, isLeaf: true,
                        aggregatedCount: pooledCount, leafReason: .pooled,
                        cushion: leafCushion))
                continue
            }
            if child < 0 {
                var leafCushion = cushion
                leafCushion.addRidge(childRect, height: childRidge)
                cells.append(
                    TreemapCell(
                        rect: childRect, node: child, depth: depth + 1, isLeaf: true,
                        cushion: leafCushion))
                continue
            }
            if arena.isDirectory(Int(child)) {
                recurse(
                    arena: arena, node: child, rect: childRect, depth: depth + 1,
                    options: options, cushion: cushion, ridge: childRidge, into: &cells)
            } else {
                var leafCushion = cushion
                leafCushion.addRidge(childRect, height: childRidge)
                cells.append(
                    TreemapCell(
                        rect: childRect, node: child, depth: depth + 1, isLeaf: true,
                        cushion: leafCushion))
            }
        }
    }

    /// Fills one cell, lighting its accumulated cushion surface.
    ///
    /// Because the surface carries a ridge from every enclosing folder, the shading
    /// itself shows the directory structure — which is what borders would otherwise
    /// have to stand in for.
    @inline(__always)
    private static func paint(
        _ cell: TreemapCell, colour: UInt32, index: Int,
        px: UnsafeMutableBufferPointer<UInt32>, idb: UnsafeMutableBufferPointer<Int32>,
        width: Int, height: Int
    ) {
        // Round both edges the same way so cells tile exactly. Expanding outward
        // (floor/ceil) makes neighbours overlap.
        let x0 = max(0, Int(cell.rect.minX.rounded()))
        let y0 = max(0, Int(cell.rect.minY.rounded()))
        let x1 = min(width, Int(cell.rect.maxX.rounded()))
        let y1 = min(height, Int(cell.rect.maxY.rounded()))
        guard x1 > x0, y1 > y0 else { return }

        let baseR = Double((colour >> 16) & 0xFF)
        let baseG = Double((colour >> 8) & 0xFF)
        let baseB = Double(colour & 0xFF)
        let cushion = cell.cushion
        let ambient = ambientLight
        let diffuse = 1 - ambient
        let gain = renderGain

        for y in y0..<y1 {
            let row = y * width
            let ny0 = -(2 * cushion.b * (Double(y) + 0.5) + cushion.d)
            for x in x0..<x1 {
                let nx = -(2 * cushion.a * (Double(x) + 0.5) + cushion.c)
                var cosa =
                    (nx * lightX + ny0 * lightY + lightZ)
                    / (nx * nx + ny0 * ny0 + 1).squareRoot()
                if cosa < 0 { cosa = 0 }
                // Slight gain so mid-tones do not sit dark; a steep surface
                // otherwise leaves most of a large cushion dim.
                let intensity = min(1.0, (ambient + diffuse * cosa) * gain)
                let r = UInt32(min(255, max(0, baseR * intensity)))
                let g = UInt32(min(255, max(0, baseG * intensity)))
                let b = UInt32(min(255, max(0, baseB * intensity)))
                px[row + x] = 0xFF00_0000 | (r << 16) | (g << 8) | b
                idb[row + x] = Int32(index)
            }
        }
    }

    /// A colour for a directory we cannot recurse into.
    ///
    /// Follows the largest child down until it reaches a file, so a collapsed
    /// folder takes on the colour of whatever dominates it rather than rendering
    /// as an anonymous grey block. Both the descent and the fan-out are capped so
    /// this stays cheap enough to call per cell.
    static func representativeKind(arena: NodeArena, node: Int32, metric: SizeMetric) -> Int16 {
        var current = Int(node)
        var steps = 0
        while arena.isDirectory(current), steps < 8 {
            let kids = arena.children(of: current)
            guard !kids.isEmpty else { break }
            var best = -1
            var bestValue: Int64 = -1
            var scanned = 0
            for c in kids {
                if scanned >= 256 { break }
                scanned += 1
                let v = arena.size(metric, c)
                if v > bestValue {
                    bestValue = v
                    best = c
                }
            }
            guard best >= 0, bestValue > 0 else { break }
            current = best
            steps += 1
        }
        return arena.kind[current]
    }

    // MARK: - Rasterisation

    /// Fills the pixel buffer and the ID buffer in a single pass. Rectangles are
    /// axis-aligned, so writing pixels directly is both faster than CoreGraphics
    /// and gives us the hit-test buffer for free.
    static func render(
        arena: NodeArena, root: Int32, width: Int, height: Int, palette: KindPalette,
        options: TreemapOptions = TreemapOptions()
    ) -> TreemapImage {
        let metric = options.metric
        // Readers must never touch a node at or above the published count.
        guard width > 0, height > 0, Int(root) < arena.count else {
            return TreemapImage(
                width: 0, height: 0, cells: [], ids: [], image: nil, rootNode: root)
        }

        let cells = build(
            arena: arena, root: root,
            bounds: CGRect(x: 0, y: 0, width: Double(width), height: Double(height)),
            options: options)
        var extraByID: [Int32: ExtraBlock] = [:]
        for block in options.extraBlocks { extraByID[block.id] = block }

        var pixels = [UInt32](repeating: palette.background, count: width * height)
        var ids = [Int32](repeating: -1, count: width * height)

        pixels.withUnsafeMutableBufferPointer { px in
            ids.withUnsafeMutableBufferPointer { idb in
                for (index, cell) in cells.enumerated() where cell.isLeaf {
                    if let block = extraByID[cell.node] {
                        paint(
                            cell, colour: block.colour, index: index, px: px, idb: idb,
                            width: width, height: height)
                        continue
                    }
                    var kind = arena.kind[Int(cell.node)]
                    if kind == FileKind.directory {
                        kind = representativeKind(arena: arena, node: cell.node, metric: metric)
                    }
                    // A folder is always shown as its contents. If even the
                    // representative lookup found nothing, fall back to a normal
                    // colour rather than painting a dark grey hole.
                    if kind == FileKind.directory { kind = FileKind.otherID }
                    paint(
                        cell, colour: palette.colour(forKind: kind), index: index, px: px,
                        idb: idb, width: width, height: height)
                }

                // Darken the perimeter of each folder's rectangle.
                //
                // Cushion shading alone leaves only a 36-54/255 step where two
                // folders meet, which disappears when both are large blocks of the
                // same colour — so a selection outline could land in the middle of
                // what looked like one continuous area. This puts a real border
                // exactly on every folder boundary, fading with depth so the top
                // levels read strongest. It only modulates pixels that are already
                // painted, so nothing shows through and the ID buffer is untouched.
            }
        }

        var directoryRects: [Int32: CGRect] = [:]
        directoryRects.reserveCapacity(cells.count)
        // Pooled cells stand for many items under one representative node, so their
        // rectangle does not belong to that node and would highlight the wrong area.
        for cell in cells where cell.aggregatedCount == 0 && cell.node >= 0 {
            // Container cells carry the folder's full rectangle; prefer them, and
            // otherwise keep the largest rectangle seen for that node.
            if let existing = directoryRects[cell.node],
                existing.width * existing.height >= cell.rect.width * cell.rect.height
            {
                continue
            }
            directoryRects[cell.node] = cell.rect
        }

        let image = makeImage(pixels: pixels, width: width, height: height)
        return TreemapImage(
            width: width, height: height, cells: cells, ids: ids, image: image, rootNode: root,
            directoryRects: directoryRects, extraBlocks: extraByID)
    }



    private static func makeImage(pixels: [UInt32], width: Int, height: Int) -> CGImage? {
        // The provider must own its bytes: a pointer into a local buffer would
        // dangle the moment this function returns.
        let bytes = pixels.withUnsafeBufferPointer { Data(buffer: $0) }
        guard let provider = CGDataProvider(data: bytes as CFData) else { return nil }
        return CGImage(
            width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
            bytesPerRow: width * 4,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue)
                .union(.byteOrder32Little),
            provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}

/// Stable colour per file kind, shared by the treemap and the kind table so the
/// two columns read as one system.
final class KindPalette {
    let background: UInt32 = 0xFF1C_1C1E
    private var cache: [UInt32] = []
    /// Kinds in descending size order for the current scan, used when colours are
    /// assigned per scan rather than per kind.
    private var rankOfKind: [Int16: Int] = [:]
    /// Derive colour from the kind itself, so a file type looks the same in every
    /// scan. Off, the biggest kinds in this scan get the most distinct hues.
    var useConsistentColours = true

    /// First four palette slots are reserved for unknown/folder/symlink/no-extension.
    private static let reservedSlots = 4

    func setRanking(_ kinds: [Int16]) {
        rankOfKind = Dictionary(
            uniqueKeysWithValues: kinds.enumerated().map { ($1, $0) })
    }

    init() {
        FileKind.warmUp()
        cache = (0..<FileKind.names.count).map { Self.colour(index: $0, of: FileKind.names.count) }
    }

    func colour(forKind kind: Int16) -> UInt32 {
        if !useConsistentColours, let rank = rankOfKind[kind], cache.count > Self.reservedSlots {
            let span = cache.count - Self.reservedSlots
            return cache[Self.reservedSlots + (rank % span)]
        }
        let i = Int(kind)
        return i >= 0 && i < cache.count ? cache[i] : 0xFF80_8080
    }

    func nsColor(forKind kind: Int16) -> NSColor {
        let c = colour(forKind: kind)
        return NSColor(
            srgbRed: CGFloat((c >> 16) & 0xFF) / 255, green: CGFloat((c >> 8) & 0xFF) / 255,
            blue: CGFloat(c & 0xFF) / 255, alpha: 1)
    }

    /// Golden-ratio hue stepping keeps adjacent kinds visually distinct instead of
    /// producing the near-identical neighbours a linear ramp gives.
    private static func colour(index: Int, of total: Int) -> UInt32 {
        if index == FileKind.directory { return 0xFF3A_3A3C }
        if index == FileKind.symlink { return 0xFF5A_5A5E }
        let hue = (Double(index) * 0.6180339887).truncatingRemainder(dividingBy: 1.0)
        let saturation = 0.55 + 0.2 * Double((index % 3)) / 2.0
        let brightness = 0.72 + 0.18 * Double((index % 2))
        let c = NSColor(
            calibratedHue: hue, saturation: saturation, brightness: brightness, alpha: 1)
        guard let rgb = c.usingColorSpace(.sRGB) else { return 0xFF80_8080 }
        let r = UInt32(rgb.redComponent * 255)
        let g = UInt32(rgb.greenComponent * 255)
        let b = UInt32(rgb.blueComponent * 255)
        return 0xFF00_0000 | (r << 16) | (g << 8) | b
    }
}
