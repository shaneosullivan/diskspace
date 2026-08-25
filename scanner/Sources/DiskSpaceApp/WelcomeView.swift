import AppKit
import DiskScanCore

/// Whether the app can read everything, and how to ask if it cannot.
enum FullDiskAccess {
    /// macOS exposes no API for "do I have Full Disk Access", so probe a path that
    /// TCC protects. Readable means access was granted.
    ///
    /// Note this reflects whichever process the check runs in: launched from a
    /// terminal, the app inherits the terminal's grant rather than its own.
    static var isGranted: Bool {
        let probes = [
            NSHomeDirectory() + "/Library/Application Support/com.apple.TCC/TCC.db",
            "/Library/Application Support/com.apple.TCC/TCC.db",
        ]
        for path in probes {
            let fd = path.withCString { open($0, O_RDONLY) }
            if fd >= 0 {
                close(fd)
                return true
            }
        }
        return false
    }

    static func openSettings() {
        let url = URL(
            string:
                "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles")!
        NSWorkspace.shared.open(url)
    }
}

/// A folder or volume offered on the welcome screen.
struct ScanSuggestion {
    var title: String
    var subtitle: String
    var path: String
    var icon: NSImage
    /// Capacity and free bytes, for volumes only.
    var capacity: (total: Int64, free: Int64)?
}

enum SuggestionSource {
    /// Mounted, browsable volumes, biggest first.
    static func volumes() -> [ScanSuggestion] {
        let keys: [URLResourceKey] = [
            .volumeNameKey, .volumeTotalCapacityKey, .volumeAvailableCapacityKey,
            .volumeIsBrowsableKey, .volumeLocalizedFormatDescriptionKey,
            .volumeIsInternalKey, .volumeIsRootFileSystemKey,
        ]
        let urls =
            FileManager.default.mountedVolumeURLs(
                includingResourceValuesForKeys: keys, options: [.skipHiddenVolumes]) ?? []
        var ranked: [(rank: Int, suggestion: ScanSuggestion)] = []
        for url in urls {
            guard let values = try? url.resourceValues(forKeys: Set(keys)),
                values.volumeIsBrowsable == true,
                let total = values.volumeTotalCapacity, total > 0
            else { continue }
            let free = Int64(values.volumeAvailableCapacity ?? 0)
            let isRoot = values.volumeIsRootFileSystem ?? false
            let isInternal = values.volumeIsInternal ?? false

            // Startup disk first, then other internal disks, then anything
            // external or networked. Sorting purely by capacity would put a big
            // network share above the disk the user is actually trying to free up.
            let rank = isRoot ? 0 : (isInternal ? 1 : 2)
            var subtitle = values.volumeLocalizedFormatDescription ?? url.path
            if rank == 2 {
                subtitle += " · external or network volume, slower to scan"
            }
            ranked.append(
                (rank,
                 ScanSuggestion(
                    title: values.volumeName ?? url.lastPathComponent,
                    subtitle: subtitle,
                    path: url.path,
                    icon: NSWorkspace.shared.icon(forFile: url.path),
                    capacity: (Int64(total), free))))
        }
        return ranked
            .sorted {
                $0.rank != $1.rank
                    ? $0.rank < $1.rank
                    : ($0.suggestion.capacity?.total ?? 0) > ($1.suggestion.capacity?.total ?? 0)
            }
            .map(\.suggestion)
    }

    /// Folders that are usually where the space actually went.
    static func folders() -> [ScanSuggestion] {
        let home = NSHomeDirectory()
        let candidates: [(String, String, String)] = [
            ("Home Folder", "Everything under your user account", home),
            ("Downloads", "Often the fastest win", home + "/Downloads"),
            ("Library", "Caches, containers and application support", home + "/Library"),
            ("Developer", "Xcode archives, simulators and derived data",
             home + "/Library/Developer"),
            ("Applications", "Installed apps", "/Applications"),
        ]
        return candidates.compactMap { title, subtitle, path in
            var isDir: ObjCBool = false
            guard FileManager.default.fileExists(atPath: path, isDirectory: &isDir),
                isDir.boolValue
            else { return nil }
            return ScanSuggestion(
                title: title, subtitle: subtitle, path: path,
                icon: NSWorkspace.shared.icon(forFile: path), capacity: nil)
        }
    }
}

/// One clickable suggestion row.
final class SuggestionRow: NSView {
    private let suggestion: ScanSuggestion
    private let onPick: (String) -> Void
    private var hovered = false
    private var trackingArea: NSTrackingArea?

    init(suggestion: ScanSuggestion, onPick: @escaping (String) -> Void) {
        self.suggestion = suggestion
        self.onPick = onPick
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = 6
        translatesAutoresizingMaskIntoConstraints = false
        build()
    }

    required init?(coder: NSCoder) { fatalError() }

    private func build() {
        let iconView = NSImageView(image: suggestion.icon)
        iconView.imageScaling = .scaleProportionallyUpOrDown
        iconView.translatesAutoresizingMaskIntoConstraints = false

        let title = NSTextField(labelWithString: suggestion.title)
        title.font = .systemFont(ofSize: 13, weight: .semibold)
        title.translatesAutoresizingMaskIntoConstraints = false

        let subtitle = NSTextField(labelWithString: suggestion.subtitle)
        subtitle.font = .systemFont(ofSize: 11)
        subtitle.textColor = .secondaryLabelColor
        subtitle.lineBreakMode = .byTruncatingTail
        subtitle.translatesAutoresizingMaskIntoConstraints = false

        addSubview(iconView)
        addSubview(title)
        addSubview(subtitle)

        NSLayoutConstraint.activate([
            heightAnchor.constraint(equalToConstant: 52),
            iconView.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            iconView.centerYAnchor.constraint(equalTo: centerYAnchor),
            iconView.widthAnchor.constraint(equalToConstant: 32),
            iconView.heightAnchor.constraint(equalToConstant: 32),
            title.leadingAnchor.constraint(equalTo: iconView.trailingAnchor, constant: 10),
            title.topAnchor.constraint(equalTo: topAnchor, constant: 9),
            subtitle.leadingAnchor.constraint(equalTo: title.leadingAnchor),
            subtitle.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 2),
        ])

        if let capacity = suggestion.capacity {
            let used = capacity.total - capacity.free
            let detail = NSTextField(
                labelWithString:
                    "\(Format.bytes(used)) used of \(Format.bytes(capacity.total))  ·  \(Format.bytes(capacity.free)) free"
            )
            detail.font = .systemFont(ofSize: 11)
            detail.textColor = .secondaryLabelColor
            detail.translatesAutoresizingMaskIntoConstraints = false

            let track = NSView()
            track.wantsLayer = true
            track.layer?.backgroundColor = NSColor.quaternaryLabelColor.cgColor
            track.layer?.cornerRadius = 3
            track.translatesAutoresizingMaskIntoConstraints = false

            let fill = NSView()
            fill.wantsLayer = true
            let fraction = max(0.01, min(1.0, Double(used) / Double(max(capacity.total, 1))))
            // Red when the volume is nearly full, which is when someone opens this app.
            fill.layer?.backgroundColor =
                (fraction > 0.9 ? NSColor.systemRed : NSColor.controlAccentColor).cgColor
            fill.layer?.cornerRadius = 3
            fill.translatesAutoresizingMaskIntoConstraints = false

            addSubview(detail)
            addSubview(track)
            track.addSubview(fill)
            subtitle.isHidden = true

            NSLayoutConstraint.activate([
                detail.leadingAnchor.constraint(equalTo: title.leadingAnchor),
                detail.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 2),
                track.leadingAnchor.constraint(equalTo: title.leadingAnchor),
                track.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
                track.topAnchor.constraint(equalTo: detail.bottomAnchor, constant: 5),
                track.heightAnchor.constraint(equalToConstant: 6),
                fill.leadingAnchor.constraint(equalTo: track.leadingAnchor),
                fill.topAnchor.constraint(equalTo: track.topAnchor),
                fill.bottomAnchor.constraint(equalTo: track.bottomAnchor),
                fill.widthAnchor.constraint(
                    equalTo: track.widthAnchor, multiplier: fraction),
            ])
        } else {
            subtitle.trailingAnchor.constraint(
                lessThanOrEqualTo: trailingAnchor, constant: -12).isActive = true
        }

        let click = NSClickGestureRecognizer(target: self, action: #selector(picked))
        addGestureRecognizer(click)
    }

    @objc private func picked() { onPick(suggestion.path) }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = trackingArea { removeTrackingArea(t) }
        let t = NSTrackingArea(
            rect: bounds, options: [.mouseEnteredAndExited, .activeInKeyWindow], owner: self)
        addTrackingArea(t)
        trackingArea = t
    }

    override func mouseEntered(with event: NSEvent) {
        hovered = true
        layer?.backgroundColor = NSColor.selectedContentBackgroundColor.withAlphaComponent(0.25).cgColor
    }

    override func mouseExited(with event: NSEvent) {
        hovered = false
        layer?.backgroundColor = NSColor.clear.cgColor
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: .pointingHand)
    }
}

/// Shown until a scan has been started, in place of the three empty panes.
final class WelcomeView: NSView {
    private let onPick: (String) -> Void
    private let onBrowse: () -> Void
    private let stack = NSStackView()

    init(onPick: @escaping (String) -> Void, onBrowse: @escaping () -> Void) {
        self.onPick = onPick
        self.onBrowse = onBrowse
        super.init(frame: .zero)
        translatesAutoresizingMaskIntoConstraints = false
        build()
    }

    required init?(coder: NSCoder) { fatalError() }

    /// Re-evaluates Full Disk Access, so returning from System Settings updates the
    /// banner without needing a relaunch.
    func refreshAccessState() {
        build()
    }

    private func build() {
        subviews.forEach { $0.removeFromSuperview() }

        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false

        let content = NSStackView()
        content.orientation = .vertical
        content.alignment = .leading
        content.spacing = 8
        content.edgeInsets = NSEdgeInsets(top: 28, left: 24, bottom: 28, right: 24)
        content.translatesAutoresizingMaskIntoConstraints = false

        let heading = NSTextField(labelWithString: "Choose something to measure")
        heading.font = .systemFont(ofSize: 22, weight: .semibold)
        content.addArrangedSubview(heading)

        let sub = NSTextField(
            labelWithString: "Scanning is incremental — results appear as they are found.")
        sub.font = .systemFont(ofSize: 12)
        sub.textColor = .secondaryLabelColor
        content.addArrangedSubview(sub)
        content.setCustomSpacing(20, after: sub)

        if !FullDiskAccess.isGranted {
            content.addArrangedSubview(makeAccessBanner())
        }

        let volumes = SuggestionSource.volumes()
        if !volumes.isEmpty {
            content.addArrangedSubview(sectionHeader("Volumes"))
            for v in volumes {
                content.addArrangedSubview(SuggestionRow(suggestion: v, onPick: onPick))
            }
        }

        let folders = SuggestionSource.folders()
        if !folders.isEmpty {
            content.addArrangedSubview(sectionHeader("Folders"))
            for f in folders {
                content.addArrangedSubview(SuggestionRow(suggestion: f, onPick: onPick))
            }
        }

        let browse = NSButton(title: "Choose Another Folder…", target: self, action: #selector(browse))
        browse.bezelStyle = .rounded
        browse.translatesAutoresizingMaskIntoConstraints = false
        content.setCustomSpacing(18, after: content.arrangedSubviews.last ?? browse)
        content.addArrangedSubview(browse)

        scroll.documentView = content
        addSubview(scroll)
        NSLayoutConstraint.activate([
            scroll.topAnchor.constraint(equalTo: topAnchor),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),
            content.widthAnchor.constraint(equalTo: scroll.widthAnchor),
        ])

        // Rows fill the available width so their hover highlight reads as a row.
        for row in content.arrangedSubviews where row is SuggestionRow {
            row.widthAnchor.constraint(equalTo: content.widthAnchor, constant: -48).isActive = true
        }
    }

    private func sectionHeader(_ text: String) -> NSView {
        let label = NSTextField(labelWithString: text.uppercased())
        label.font = .systemFont(ofSize: 10, weight: .semibold)
        label.textColor = .tertiaryLabelColor
        let container = NSView()
        container.translatesAutoresizingMaskIntoConstraints = false
        label.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(label)
        NSLayoutConstraint.activate([
            container.heightAnchor.constraint(equalToConstant: 28),
            label.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 10),
            label.bottomAnchor.constraint(equalTo: container.bottomAnchor, constant: -4),
        ])
        return container
    }

    private func makeAccessBanner() -> NSView {
        let box = NSView()
        box.wantsLayer = true
        box.layer?.backgroundColor = NSColor.systemYellow.withAlphaComponent(0.13).cgColor
        box.layer?.cornerRadius = 8
        box.translatesAutoresizingMaskIntoConstraints = false

        let title = NSTextField(labelWithString: "Some folders can’t be read")
        title.font = .systemFont(ofSize: 13, weight: .semibold)
        title.translatesAutoresizingMaskIntoConstraints = false

        let body = NSTextField(
            wrappingLabelWithString:
                "Without Full Disk Access, protected folders are skipped and totals will be lower than the real figure. Scanning still works.")
        body.font = .systemFont(ofSize: 11)
        body.textColor = .secondaryLabelColor
        body.translatesAutoresizingMaskIntoConstraints = false

        let button = NSButton(
            title: "Grant Full Disk Access…", target: self, action: #selector(requestAccess))
        button.bezelStyle = .rounded
        button.translatesAutoresizingMaskIntoConstraints = false

        box.addSubview(title)
        box.addSubview(body)
        box.addSubview(button)
        NSLayoutConstraint.activate([
            title.topAnchor.constraint(equalTo: box.topAnchor, constant: 12),
            title.leadingAnchor.constraint(equalTo: box.leadingAnchor, constant: 14),
            body.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 4),
            body.leadingAnchor.constraint(equalTo: title.leadingAnchor),
            body.trailingAnchor.constraint(equalTo: box.trailingAnchor, constant: -14),
            button.topAnchor.constraint(equalTo: body.bottomAnchor, constant: 10),
            button.leadingAnchor.constraint(equalTo: title.leadingAnchor),
            button.bottomAnchor.constraint(equalTo: box.bottomAnchor, constant: -12),
        ])
        return box
    }

    @objc private func requestAccess() {
        FullDiskAccess.openSettings()
        // The grant only takes effect on relaunch, so say so rather than leaving
        // the user wondering why the banner is still there.
        let alert = NSAlert()
        alert.messageText = "Grant access, then relaunch"
        alert.informativeText =
            "Add Disk Space to Full Disk Access in System Settings, then quit and reopen the app for the change to take effect."
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }

    @objc private func browse() { onBrowse() }
}
