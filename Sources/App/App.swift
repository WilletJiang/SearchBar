import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var index: FileIndex!
    private var providers: SystemProviders!
    private var coordinator: SearchCoordinator!
    private var panel: PanelController!
    private var preferences: PreferencesController!
    private let hotkey = HotKey()
    private var item: NSStatusItem!
    private var searchMenuItem: NSMenuItem!
#if SEARCHBAR_BENCHMARK
    private var benchmark: AppPerformance?
#endif

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        Preferences.shared.appearanceMode.apply()
        searchMenuItem = AppMenu.install(target: self, search: #selector(toggleSearch), settings: #selector(showPreferences), quit: #selector(quit))
        let arguments = CommandLine.arguments
#if SEARCHBAR_BENCHMARK
        func option(_ name: String) -> String? {
            guard let flag = arguments.firstIndex(of: name), arguments.indices.contains(flag + 1) else { return nil }
            return arguments[flag + 1]
        }
        let benchmarkHome = option("--benchmark-home").map { URL(fileURLWithPath: $0) }
        index = FileIndex(home: benchmarkHome ?? FileManager.default.homeDirectoryForCurrentUser,
                          storage: option("--benchmark-storage").map { URL(fileURLWithPath: $0) } ?? FileIndex.storageURL,
                          applications: benchmarkHome == nil ? nil : [])
#else
        index = FileIndex()
#endif
        providers = SystemProviders()
        coordinator = SearchCoordinator(index: index, providers: providers)
        panel = PanelController(coordinator: coordinator)
        preferences = PreferencesController(index: index, providers: providers, hotkey: hotkey)
        panel.onSettings = { [weak self] in self?.panel.hide(); self?.preferences.show() }
        index.onStatus = { [weak self] text, _ in
            self?.panel.updateStatus(text)
            self?.preferences.updateStatus()
        }
        hotkey.onPress = { [weak self] in self?.panel.toggle() }
        panel.isToggleShortcut = { [weak self] event in self?.hotkey.matches(event) == true }
        hotkey.register(choice: Preferences.shared.hotkey)
        preferences.onHotkeyChanged = { [weak self] in
            self?.updateHotkey()
        }
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.image = Self.makeStatusBarIcon()
        let menu = NSMenu()
        menu.addItem(withTitle: "Search…", action: #selector(showSearch), keyEquivalent: "").target = self
        menu.addItem(withTitle: "Settings…", action: #selector(showPreferences), keyEquivalent: ",").target = self
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit SearchBar", action: #selector(quit), keyEquivalent: "q").target = self
        item.menu = menu
        updateHotkey()
        providers.prepare()
#if SEARCHBAR_BENCHMARK
        panel.onMetric = { [weak self] name, value in
            self?.benchmark?.record(name, value)
        }
        if let flag = arguments.firstIndex(of: "--benchmark"), arguments.indices.contains(flag + 1) {
            benchmark = AppPerformance(panel: panel, index: index, output: arguments[flag + 1])
            benchmark?.start()
            if arguments.contains("--benchmark-no-scan") { index.prepareBenchmarkIndex() }
            else { index.start() }
            return
        }
#endif
        index.start()
        if let flag = arguments.firstIndex(of: "--query"), arguments.indices.contains(flag + 1) { panel.show(query: arguments[flag + 1]) }
        else if !arguments.contains("--background") { panel.show() }
    }

    private func updateHotkey() {
        item.button?.toolTip = "SearchBar · " + hotkey.description
        searchMenuItem.keyEquivalent = hotkey.activeChoice == nil ? "" : " "
        searchMenuItem.keyEquivalentModifierMask = hotkey.menuModifiers
    }

    private static func makeStatusBarIcon() -> NSImage {
        let size = NSSize(width: 18, height: 18)
        let image = NSImage(size: size, flipped: true) { _ in
            let scale: CGFloat = 18.0 / 24.0
            func pt(_ x: CGFloat, _ y: CGFloat) -> NSPoint {
                NSPoint(x: x * scale, y: y * scale)
            }
            func quad(on path: NSBezierPath, from p0: NSPoint, control c: NSPoint, to p2: NSPoint) {
                let c1 = NSPoint(x: p0.x + (c.x - p0.x) * (2.0 / 3.0), y: p0.y + (c.y - p0.y) * (2.0 / 3.0))
                let c2 = NSPoint(x: p2.x + (c.x - p2.x) * (2.0 / 3.0), y: p2.y + (c.y - p2.y) * (2.0 / 3.0))
                path.curve(to: p2, controlPoint1: c1, controlPoint2: c2)
            }

            let stroke = NSBezierPath()
            stroke.lineWidth = 1.95 * scale
            stroke.lineCapStyle = .round
            stroke.lineJoinStyle = .round

            // Rounded hexagonal optical prism (24x24 grid)
            stroke.move(to: pt(10.788, 3.500))
            quad(on: stroke, from: pt(10.788, 3.500), control: pt(12.000, 2.800), to: pt(13.212, 3.500))
            stroke.line(to: pt(18.755, 6.700))
            quad(on: stroke, from: pt(18.755, 6.700), control: pt(19.967, 7.400), to: pt(19.967, 8.800))
            stroke.line(to: pt(19.967, 15.200))
            quad(on: stroke, from: pt(19.967, 15.200), control: pt(19.967, 16.600), to: pt(18.755, 17.300))
            stroke.line(to: pt(13.212, 20.500))
            quad(on: stroke, from: pt(13.212, 20.500), control: pt(12.000, 21.200), to: pt(10.788, 20.500))
            stroke.line(to: pt(5.245, 17.300))
            quad(on: stroke, from: pt(5.245, 17.300), control: pt(4.033, 16.600), to: pt(4.033, 15.200))
            stroke.line(to: pt(4.033, 8.800))
            quad(on: stroke, from: pt(4.033, 8.800), control: pt(4.033, 7.400), to: pt(5.245, 6.700))
            stroke.close()

            // Internal refraction facets
            stroke.move(to: pt(12.0, 3.4))
            stroke.line(to: pt(16.8, 12.0))
            stroke.line(to: pt(12.0, 20.6))

            stroke.move(to: pt(4.4, 7.8))
            stroke.line(to: pt(16.8, 12.0))
            stroke.line(to: pt(4.4, 16.2))

            NSColor.black.setStroke()
            stroke.stroke()
            return true
        }
        image.isTemplate = true
        image.accessibilityDescription = "SearchBar"
        return image
    }

    @objc private func showSearch() { panel.show() }
    @objc private func toggleSearch() { panel.toggle() }
    @objc private func showPreferences() { panel.hide(); preferences.show() }
    @objc private func quit() { NSApp.terminate(nil) }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if panel?.isPresented != true { panel?.show() }
        return true
    }
    func applicationWillTerminate(_ notification: Notification) {
        panel.hide(immediately: true)
        providers.cancelSearch()
        index.stop()
    }
}

@main
enum SearchBarApp {
    static func main() {
        if ScanWorker.runIfRequested() { return }
        let application = NSApplication.shared
        let delegate = AppDelegate()
        application.delegate = delegate
        withExtendedLifetime(delegate) { application.run() }
    }
}
