import AppKit
import DiskScanCore

/// Development-only diagnostics.
///
/// Everything here is inert unless `DISKSPACE_DEBUG=1` is set in the environment,
/// so a normal build writes nothing and pays only a boolean check. The point is to
/// capture the state a bug report needs — what the map drew, and everything known
/// about the selected item — instead of reconstructing it after the fact.
enum DebugLog {
    static let isEnabled: Bool = ProcessInfo.processInfo.environment["DISKSPACE_DEBUG"] == "1"

    static let directory: URL = {
        let override = ProcessInfo.processInfo.environment["DISKSPACE_DEBUG_DIR"]
        let base =
            override.map { URL(fileURLWithPath: $0) }
            ?? FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("Logs/DiskSpace", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }()

    private static let queue = DispatchQueue(label: "debug-log")
    private static let started = Date()

    private static let stamp: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    /// Appends a line to session.log.
    static func line(_ text: String) {
        guard isEnabled else { return }
        let entry = "[\(stamp.string(from: Date()))] \(text)\n"
        queue.async {
            let url = directory.appendingPathComponent("session.log")
            if let handle = try? FileHandle(forWritingTo: url) {
                handle.seekToEndOfFile()
                handle.write(Data(entry.utf8))
                try? handle.close()
            } else {
                try? Data(entry.utf8).write(to: url)
            }
        }
    }

    static func writeJSON(_ object: Any, to name: String) {
        guard isEnabled else { return }
        queue.async {
            guard
                let data = try? JSONSerialization.data(
                    withJSONObject: object, options: [.prettyPrinted, .sortedKeys])
            else {
                line("failed to encode \(name)")
                return
            }
            let url = directory.appendingPathComponent(name)
            try? data.write(to: url)
            line("wrote \(name) (\(data.count) bytes)")
        }
    }

    static func announce() {
        guard isEnabled else { return }
        line("--- debug session started, logs in \(directory.path) ---")
    }

    // MARK: - Treemap dump

    /// Everything the renderer produced: one entry per cell, plus the render's
    /// parameters. Written once a scan finishes, so a map can be inspected
    /// offline rather than described from a screenshot.
    static func dumpTreemap(
        _ image: TreemapImage, arena: NodeArena, palette: KindPalette, options: TreemapOptions,
        rootPath: String
    ) {
        guard isEnabled else { return }
        var cells: [[Any]] = []
        cells.reserveCapacity(image.cells.count)
        for cell in image.cells {
            let node = Int(cell.node)
            let name = node >= 0 && node < arena.count ? arena.name(node) : "«extra»"
            let kind = node >= 0 && node < arena.count ? Int(arena.kind[node]) : -1
            let colour =
                node >= 0 && node < arena.count
                ? Int(palette.colour(forKind: arena.kind[node]) & 0xFF_FFFF) : -1
            // Compact rows keep a 150k-cell dump readable and small enough to grep.
            cells.append([
                node, name, kind, colour, cell.isLeaf, cell.depth, Int(cell.aggregatedCount),
                Int(cell.rect.minX.rounded()), Int(cell.rect.minY.rounded()),
                Int(cell.rect.width.rounded()), Int(cell.rect.height.rounded()),
            ])
        }
        let payload: [String: Any] = [
            "columns": [
                "node", "name", "kind", "colourRGB", "isLeaf", "depth", "aggregatedCount",
                "x", "y", "w", "h",
            ],
            "rootPath": rootPath,
            "rootNode": Int(image.rootNode),
            "imageWidth": image.width,
            "imageHeight": image.height,
            "cellCount": image.cells.count,
            "options": [
                "metric": options.metric.title,
                "useLogicalSize": options.useLogicalSize,
                "expandPackages": options.expandPackages,
                "pixelScale": options.pixelScale,
                "extraBlocks": options.extraBlocks.map { ["label": $0.label, "bytes": $0.bytes] },
            ],
            "cells": cells,
        ]
        writeJSON(payload, to: "treemap.json")
    }

    // MARK: - Selection dump

    /// Everything known about the selected node, including what the map actually
    /// painted where the highlight is drawn. Overwritten on each selection.
    static func dumpSelection(
        node: Int32, arena: NodeArena, image: TreemapImage?, palette: KindPalette,
        metric: SizeMetric, displaySize: Int64
    ) {
        guard isEnabled else { return }
        let index = Int(node)
        guard index >= 0, index < arena.count else { return }

        var ancestors: [[String: Any]] = []
        var current = index
        var hops = 0
        while current > 0, hops < 256 {
            current = Int(arena.parent[current])
            ancestors.append([
                "node": current, "name": arena.name(current),
                "allocated": arena.allocated[current],
            ])
            hops += 1
        }

        var payload: [String: Any] = [
            "node": index,
            "name": arena.name(index),
            "path": arena.path(index),
            "isDirectory": arena.isDirectory(index),
            "kind": Int(arena.kind[index]),
            "kindName": FileKind.names[Int(arena.kind[index])],
            "paletteColourRGB": String(
                format: "#%06X", palette.colour(forKind: arena.kind[index]) & 0xFF_FFFF),
            "flags": Int(arena.flags[index]),
            "allocated": arena.allocated[index],
            "logical": arena.logical[index],
            "fileCount": Int(arena.fileCount[index]),
            "addedTime": Int(arena.addedTime[index]),
            "childStart": Int(arena.childStart[index]),
            "childCount": Int(arena.childCount[index]),
            "metric": metric.title,
            "displaySize": displaySize,
            "ancestors": ancestors,
            "preferences": [
                "showPackageContents": Preferences.shared.showPackageContents,
                "showPhysicalFileSize": Preferences.shared.showPhysicalFileSize,
                "consistentKindColours": Preferences.shared.consistentKindColours,
                "showFreeSpace": Preferences.shared.showFreeSpace,
                "showOtherSpace": Preferences.shared.showOtherSpace,
            ],
        ]

        if let image {
            payload["renderWidth"] = image.width
            payload["renderHeight"] = image.height
            payload["renderRootNode"] = Int(image.rootNode)

            if let rect = image.directoryRects[node] {
                payload["highlightRect"] = [
                    "x": rect.minX, "y": rect.minY, "w": rect.width, "h": rect.height,
                ]
                let cx = Int(rect.midX)
                let cy = Int(rect.midY)
                let id = image.cellIndex(atX: cx, y: cy)
                var probe: [String: Any] = ["x": cx, "y": cy, "cellIndex": Int(id)]
                if id >= 0, Int(id) < image.cells.count {
                    let painted = image.cells[Int(id)]
                    let pn = Int(painted.node)
                    probe["paintedNode"] = pn
                    probe["paintedName"] = pn >= 0 && pn < arena.count ? arena.name(pn) : "«extra»"
                    probe["matchesSelection"] = pn == index
                }
                // The colour actually on screen at that point.
                if let cg = image.image, let bytes = cg.dataProvider?.data as Data? {
                    let offset = cy * cg.bytesPerRow + cx * 4
                    if offset + 3 < bytes.count {
                        probe["paintedRGB"] = String(
                            format: "#%02X%02X%02X", bytes[offset + 2], bytes[offset + 1],
                            bytes[offset])
                    }
                }
                payload["probe"] = probe
            } else {
                payload["highlightRect"] = NSNull()
                payload["note"] = "no rectangle for this node in the current render"
            }

            // Every cell that names this node, in case more than one does.
            var matching: [[String: Any]] = []
            for (i, cell) in image.cells.enumerated() where cell.node == node {
                matching.append([
                    "cellIndex": i, "isLeaf": cell.isLeaf, "depth": cell.depth,
                    "aggregatedCount": Int(cell.aggregatedCount),
                    "x": cell.rect.minX, "y": cell.rect.minY,
                    "w": cell.rect.width, "h": cell.rect.height,
                ])
            }
            payload["cellsNamingThisNode"] = matching
        }

        writeJSON(payload, to: "selection.json")
    }
}
