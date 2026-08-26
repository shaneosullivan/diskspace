import AppKit

/// Builds the menu bar.
///
/// The app is created programmatically rather than from a nib, so every menu has
/// to be assembled here — including the standard ones AppKit would otherwise
/// provide, such as Quit and the window list.
enum MainMenu {
    static func install(target: AnyObject) {
        let bar = NSMenu()

        bar.addItem(applicationMenu(target: target))
        bar.addItem(fileMenu(target: target))
        bar.addItem(editMenu())
        bar.addItem(viewMenu(target: target))
        let window = windowMenu(target: target)
        bar.addItem(window)
        bar.addItem(helpMenu())

        NSApp.mainMenu = bar
        // Lets AppKit append the open-window list and its own window commands.
        NSApp.windowsMenu = window.submenu
    }

    private static func item(
        _ title: String, _ action: Selector?, _ key: String = "",
        modifiers: NSEvent.ModifierFlags = .command, target: AnyObject? = nil
    ) -> NSMenuItem {
        let menuItem = NSMenuItem(title: title, action: action, keyEquivalent: key)
        if !key.isEmpty { menuItem.keyEquivalentModifierMask = modifiers }
        menuItem.target = target
        return menuItem
    }

    private static func submenu(_ title: String, _ items: [NSMenuItem]) -> NSMenuItem {
        let holder = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        let menu = NSMenu(title: title)
        for i in items { menu.addItem(i) }
        holder.submenu = menu
        return holder
    }

    private static func applicationMenu(target: AnyObject) -> NSMenuItem {
        let name = "Disk Space"
        let services = NSMenu(title: "Services")
        NSApp.servicesMenu = services
        let servicesItem = NSMenuItem(title: "Services", action: nil, keyEquivalent: "")
        servicesItem.submenu = services

        return submenu(
            name,
            [
                item("About \(name)", #selector(AppCommands.showAbout(_:)), target: target),
                .separator(),
                item("Settings…", #selector(AppCommands.showPreferences(_:)), ",", target: target),
                .separator(),
                servicesItem,
                .separator(),
                item("Hide \(name)", #selector(NSApplication.hide(_:)), "h"),
                item(
                    "Hide Others", #selector(NSApplication.hideOtherApplications(_:)), "h",
                    modifiers: [.command, .option]),
                item("Show All", #selector(NSApplication.unhideAllApplications(_:))),
                .separator(),
                item("Quit \(name)", #selector(NSApplication.terminate(_:)), "q"),
            ])
    }

    private static func fileMenu(target: AnyObject) -> NSMenuItem {
        submenu(
            "File",
            [
                item("Open…", #selector(AppCommands.openFolder(_:)), "o", target: target),
                .separator(),
                item("Close", #selector(NSWindow.performClose(_:)), "w"),
            ])
    }

    private static func editMenu() -> NSMenuItem {
        submenu(
            "Edit",
            [
                item("Undo", Selector(("undo:")), "z"),
                item("Redo", Selector(("redo:")), "z", modifiers: [.command, .shift]),
                .separator(),
                item("Cut", #selector(NSText.cut(_:)), "x"),
                item("Copy", #selector(NSText.copy(_:)), "c"),
                item("Paste", #selector(NSText.paste(_:)), "v"),
                item("Select All", #selector(NSText.selectAll(_:)), "a"),
            ])
    }

    private static func viewMenu(target: AnyObject) -> NSMenuItem {
        let zoomOutTo = NSMenuItem(title: "Zoom Out To", action: nil, keyEquivalent: "")
        let zoomOutToMenu = NSMenu(title: "Zoom Out To")
        // Filled in on demand from the current zoom stack.
        zoomOutToMenu.delegate = target as? NSMenuDelegate
        zoomOutTo.submenu = zoomOutToMenu

        return submenu(
            "View",
            [
                item("Zoom In", #selector(AppCommands.zoomIn(_:)), "+", target: target),
                item("Zoom Out", #selector(AppCommands.zoomOut(_:)), "-", target: target),
                zoomOutTo,
                .separator(),
                item(
                    "Select Parent Folder", #selector(AppCommands.selectParentFolder(_:)), "u",
                    target: target),
                .separator(),
                item("Show Free Space", #selector(AppCommands.toggleFreeSpace(_:)), target: target),
                item(
                    "Show Other Space", #selector(AppCommands.toggleOtherSpace(_:)), target: target),
                .separator(),
                item(
                    "Show Package Contents", #selector(AppCommands.togglePackageContents(_:)),
                    target: target),
                item(
                    "Show Logical File Size", #selector(AppCommands.toggleLogicalSize(_:)),
                    target: target),
                .separator(),
                item(
                    "Split Horizontally", #selector(AppCommands.toggleSplitHorizontally(_:)),
                    target: target),
                .separator(),
                item(
                    "Enter Full Screen", #selector(NSWindow.toggleFullScreen(_:)), "f",
                    modifiers: [.control, .command]),
            ])
    }

    private static func windowMenu(target: AnyObject) -> NSMenuItem {
        submenu(
            "Window",
            [
                item("Minimize", #selector(NSWindow.performMiniaturize(_:)), "m"),
                item("Zoom", #selector(NSWindow.performZoom(_:))),
                .separator(),
                item(
                    "Show File Kind Statistics", #selector(AppCommands.toggleKindStatistics(_:)),
                    target: target),
                item(
                    "Show Selection List", #selector(AppCommands.toggleSelectionList(_:)),
                    target: target),
                .separator(),
                item("Bring All to Front", #selector(NSApplication.arrangeInFront(_:))),
            ])
    }

    private static func helpMenu() -> NSMenuItem {
        let menu = submenu("Help", [])
        NSApp.helpMenu = menu.submenu
        return menu
    }
}

/// Commands the menu bar sends. Implemented by the main window controller.
@objc protocol AppCommands {
    func showAbout(_ sender: Any?)
    func showPreferences(_ sender: Any?)
    func openFolder(_ sender: Any?)
    func zoomIn(_ sender: Any?)
    func zoomOut(_ sender: Any?)
    func zoomOutTo(_ sender: Any?)
    func selectParentFolder(_ sender: Any?)
    func toggleFreeSpace(_ sender: Any?)
    func toggleOtherSpace(_ sender: Any?)
    func togglePackageContents(_ sender: Any?)
    func toggleLogicalSize(_ sender: Any?)
    func toggleSplitHorizontally(_ sender: Any?)
    func toggleKindStatistics(_ sender: Any?)
    func toggleSelectionList(_ sender: Any?)
    func toggleInformation(_ sender: Any?)
}
