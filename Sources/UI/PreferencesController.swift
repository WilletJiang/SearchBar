import AppKit
import ServiceManagement

final class PreferencesController: NSObject {
    private var window: NSWindow?
    private let index: FileIndex
    private let providers: SystemProviders
    private let hotkey: HotKey

    private let shortcut = NSPopUpButton()
    private let shortcutCaption = NSTextField(wrappingLabelWithString: "")
    private let loginSwitch = NSSwitch()

    private let appearance = NSPopUpButton()
    private let backgroundRow = NSStackView()
    private let appearanceCaption = NSTextField(wrappingLabelWithString: "")

    private let contentSwitch = NSSwitch()
    private let excludedSubtitle = NSTextField(labelWithString: "")
    private let clearExclusionsButton = NSButton()
    private let indexSubtitle = NSTextField(labelWithString: "")

    private let contactsSubtitle = NSTextField(labelWithString: "")
    private let footerLabel = NSTextField(labelWithString: "All queries and indexed content stay on this Mac.")

    var onHotkeyChanged: (() -> Void)?

    init(index: FileIndex, providers: SystemProviders, hotkey: HotKey) {
        self.index = index
        self.providers = providers
        self.hotkey = hotkey
        super.init()
    }

    func show() {
        if window == nil { build() }
        refresh()
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
    }

    func updateStatus() {
        if window?.isVisible == true { refresh() }
    }

    private func build() {
        let width: CGFloat = 460
        let contentWidth = width - 40
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: width, height: 520),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = "SearchBar Settings"
        window.isReleasedWhenClosed = false
        self.window = window

        let root = NSView()
        root.translatesAutoresizingMaskIntoConstraints = false
        window.contentView = root

        let mainStack = NSStackView()
        mainStack.orientation = .vertical
        mainStack.alignment = .leading
        mainStack.spacing = 18
        mainStack.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(mainStack)

        NSLayoutConstraint.activate([
            mainStack.topAnchor.constraint(equalTo: root.topAnchor, constant: 18),
            mainStack.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 20),
            mainStack.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -20),
            mainStack.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -18),
            mainStack.widthAnchor.constraint(equalToConstant: contentWidth)
        ])

        // 1. General
        shortcut.addItems(withTitles: HotKey.titles)
        shortcut.controlSize = .small
        shortcut.target = self
        shortcut.action = #selector(shortcutChanged)
        shortcut.widthAnchor.constraint(equalToConstant: 108).isActive = true

        let shortcutsButton = makeButton("System…", action: #selector(openShortcuts))
        loginSwitch.controlSize = .small
        loginSwitch.target = self
        loginSwitch.action = #selector(loginChanged)

        let generalCard = SettingsCard(rows: [
            makeRow(title: "Shortcut", trailing: makeTrailingGroup([shortcut, shortcutsButton])),
            makeRow(title: "Launch at Login", trailing: loginSwitch)
        ])
        configureCaption(shortcutCaption)
        addSection(to: mainStack, width: contentWidth, title: "General", card: generalCard, caption: shortcutCaption)

        // 2. Appearance
        appearance.addItems(withTitles: AppearanceMode.allCases.map(\.title))
        appearance.controlSize = .small
        appearance.target = self
        appearance.action = #selector(appearanceChanged)
        appearance.widthAnchor.constraint(equalToConstant: 108).isActive = true

        let allowButton = makeButton("Allow…", action: #selector(allowBackground))
        let bgRowContent = makeRow(title: "Background Sampling", trailing: allowButton)
        let bgDivider = SettingsDivider()
        backgroundRow.orientation = .vertical
        backgroundRow.alignment = .leading
        backgroundRow.spacing = 0
        backgroundRow.addArrangedSubview(bgDivider)
        backgroundRow.addArrangedSubview(bgRowContent)
        bgDivider.widthAnchor.constraint(equalTo: backgroundRow.widthAnchor).isActive = true
        bgRowContent.widthAnchor.constraint(equalTo: backgroundRow.widthAnchor).isActive = true

        let appearanceCard = SettingsCard(
            rows: [makeRow(title: "Theme", trailing: appearance)],
            extraViews: [backgroundRow]
        )
        configureCaption(appearanceCaption)
        addSection(to: mainStack, width: contentWidth, title: "Appearance", card: appearanceCard, caption: appearanceCaption)

        // 3. Indexing
        contentSwitch.controlSize = .small
        contentSwitch.target = self
        contentSwitch.action = #selector(contentChanged)

        let addExclusionButton = makeButton("Add…", action: #selector(addExclusion))
        clearExclusionsButton.title = "Clear"
        clearExclusionsButton.bezelStyle = .rounded
        clearExclusionsButton.controlSize = .small
        clearExclusionsButton.target = self
        clearExclusionsButton.action = #selector(clearExclusions)

        let rescanButton = makeButton("Rescan", action: #selector(rescan))
        let showFolderButton = makeButton("Show", action: #selector(openStorage))

        let indexingCard = SettingsCard(rows: [
            makeRow(title: "File Contents", subtitle: "128 MB on-device cache", trailing: contentSwitch),
            makeRow(title: "Excluded Folders", subtitleField: excludedSubtitle, trailing: makeTrailingGroup([addExclusionButton, clearExclusionsButton])),
            makeRow(title: "Local Index", subtitleField: indexSubtitle, trailing: makeTrailingGroup([rescanButton, showFolderButton]))
        ])
        addSection(to: mainStack, width: contentWidth, title: "Indexing", card: indexingCard)

        // 4. Permissions
        let contactsButton = makeButton("Enable", action: #selector(contacts))
        let diskButton = makeButton("Open", action: #selector(diskAccess))
        let permissionsCard = SettingsCard(rows: [
            makeRow(title: "Contacts", subtitleField: contactsSubtitle, trailing: contactsButton),
            makeRow(title: "Full Disk Access", subtitle: "Protected folders", trailing: diskButton)
        ])
        addSection(to: mainStack, width: contentWidth, title: "Permissions", card: permissionsCard)

        footerLabel.font = .systemFont(ofSize: 11)
        footerLabel.textColor = .tertiaryLabelColor
        footerLabel.alignment = .center
        footerLabel.translatesAutoresizingMaskIntoConstraints = false
        mainStack.addArrangedSubview(footerLabel)
        footerLabel.widthAnchor.constraint(equalToConstant: contentWidth).isActive = true

        NotificationCenter.default.addObserver(
            self,
            selector: #selector(windowBecameKey),
            name: NSWindow.didBecomeKeyNotification,
            object: window
        )

        resizeWindowToFit()
        window.center()
    }

    private func addSection(to parent: NSStackView, width: CGFloat, title: String, card: NSView, caption: NSTextField? = nil) {
        let inset: CGFloat = 12
        let label = NSTextField(labelWithString: title)
        label.font = .systemFont(ofSize: 11.5, weight: .semibold)
        label.textColor = .secondaryLabelColor
        let headerContainer = insetView(label, horizontal: inset)

        var views: [NSView] = [headerContainer, card]
        var captionContainer: NSView?
        if let caption {
            let container = insetView(caption, horizontal: inset)
            views.append(container)
            captionContainer = container
        }

        let stack = NSStackView(views: views)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false
        parent.addArrangedSubview(stack)

        NSLayoutConstraint.activate([
            stack.widthAnchor.constraint(equalToConstant: width),
            headerContainer.widthAnchor.constraint(equalToConstant: width),
            card.widthAnchor.constraint(equalToConstant: width)
        ])
        if let captionContainer {
            captionContainer.widthAnchor.constraint(equalToConstant: width).isActive = true
        }
    }

    private func insetView(_ view: NSView, horizontal: CGFloat) -> NSView {
        let container = NSView()
        container.translatesAutoresizingMaskIntoConstraints = false
        view.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(view)
        NSLayoutConstraint.activate([
            view.topAnchor.constraint(equalTo: container.topAnchor),
            view.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            view.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: horizontal),
            view.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -horizontal)
        ])
        return container
    }

    private func makeRow(title: String, subtitle: String? = nil, subtitleField: NSTextField? = nil, trailing: NSView) -> NSView {
        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = .systemFont(ofSize: 13)
        titleLabel.textColor = .labelColor

        let leading: NSView
        if let field = subtitleField {
            field.font = .systemFont(ofSize: 11)
            field.textColor = .secondaryLabelColor
            field.lineBreakMode = .byTruncatingTail
            let stack = NSStackView(views: [titleLabel, field])
            stack.orientation = .vertical
            stack.alignment = .leading
            stack.spacing = 1.5
            leading = stack
        } else if let subtitle {
            let sub = NSTextField(labelWithString: subtitle)
            sub.font = .systemFont(ofSize: 11)
            sub.textColor = .secondaryLabelColor
            let stack = NSStackView(views: [titleLabel, sub])
            stack.orientation = .vertical
            stack.alignment = .leading
            stack.spacing = 1.5
            leading = stack
        } else {
            leading = titleLabel
        }

        leading.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        trailing.setContentHuggingPriority(.required, for: .horizontal)
        trailing.setContentCompressionResistancePriority(.required, for: .horizontal)

        let spacer = NSView()
        spacer.setContentHuggingPriority(.fittingSizeCompression, for: .horizontal)

        let row = NSStackView(views: [leading, spacer, trailing])
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 12
        row.edgeInsets = NSEdgeInsets(top: 9, left: 12, bottom: 9, right: 12)
        row.translatesAutoresizingMaskIntoConstraints = false
        return row
    }

    private func makeTrailingGroup(_ views: [NSView]) -> NSStackView {
        let stack = NSStackView(views: views)
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 6
        return stack
    }

    private func makeButton(_ title: String, action: Selector) -> NSButton {
        let button = NSButton(title: title, target: self, action: action)
        button.bezelStyle = .rounded
        button.controlSize = .small
        return button
    }

    private func configureCaption(_ field: NSTextField) {
        field.font = .systemFont(ofSize: 11)
        field.textColor = .secondaryLabelColor
        field.maximumNumberOfLines = 2
    }

    private func resizeWindowToFit() {
        guard let window, let content = window.contentView else { return }
        content.layoutSubtreeIfNeeded()
        let fitting = content.fittingSize
        let current = window.frame
        let newFrame = window.frameRect(forContentRect: NSRect(x: 0, y: 0, width: fitting.width, height: fitting.height))
        window.setFrame(NSRect(x: current.minX, y: current.maxY - newFrame.height, width: newFrame.width, height: newFrame.height), display: true)
    }

    private func refresh() {
        shortcut.selectItem(at: Preferences.shared.hotkey)
        shortcutCaption.stringValue = hotkey.notice
        shortcutCaption.superview?.isHidden = hotkey.notice.isEmpty
        loginSwitch.state = SMAppService.mainApp.status == .enabled ? .on : .off

        appearance.selectItem(at: Preferences.shared.appearanceMode.rawValue)
        let adaptive = Preferences.shared.appearanceMode == .adaptive
        let allowed = CGPreflightScreenCaptureAccess()
        backgroundRow.isHidden = !adaptive || allowed
        if !adaptive {
            appearanceCaption.superview?.isHidden = true
        } else if allowed {
            appearanceCaption.superview?.isHidden = false
            appearanceCaption.stringValue = "Matches background brightness locally on open. No images saved."
        } else {
            appearanceCaption.superview?.isHidden = false
            appearanceCaption.stringValue = "Allow Screen Recording to match the background behind SearchBar."
        }

        contentSwitch.state = Preferences.shared.contentIndexing ? .on : .off

        let excluded = Preferences.shared.excludedPaths
        clearExclusionsButton.isHidden = excluded.isEmpty
        if excluded.isEmpty {
            excludedSubtitle.stringValue = "None"
        } else if excluded.count == 1 {
            excludedSubtitle.stringValue = (excluded[0] as NSString).abbreviatingWithTildeInPath
        } else {
            excludedSubtitle.stringValue = "\(excluded.count) folders"
        }

        indexSubtitle.stringValue = "\(index.status) · \(index.count.formatted()) items"

        let rawContacts = providers.status.components(separatedBy: " · ").first ?? ""
        if rawContacts.contains("loaded") {
            contactsSubtitle.stringValue = rawContacts.replacingOccurrences(of: "Contacts: ", with: "")
        } else if rawContacts.contains("allow") {
            contactsSubtitle.stringValue = "Denied in System Settings"
        } else {
            contactsSubtitle.stringValue = "Not enabled"
        }

        resizeWindowToFit()
    }

    @objc private func appearanceChanged() {
        guard let mode = AppearanceMode(rawValue: appearance.indexOfSelectedItem) else { return }
        Preferences.shared.appearanceMode = mode
        mode.apply()
        refresh()
    }

    @objc private func allowBackground() {
        if !CGRequestScreenCaptureAccess() {
            NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")!)
        }
        refresh()
    }

    @objc private func shortcutChanged() {
        let choice = shortcut.indexOfSelectedItem
        let success = hotkey.register(choice: choice)
        if success { Preferences.shared.hotkey = choice }
        onHotkeyChanged?()
        refresh()
        if !success {
            shortcutCaption.stringValue = "Shortcut registration failed. Try another combination."
            shortcutCaption.superview?.isHidden = false
            resizeWindowToFit()
        }
    }

    @objc private func windowBecameKey() {
        hotkey.register(choice: Preferences.shared.hotkey)
        onHotkeyChanged?()
        refresh()
    }

    @objc private func contentChanged(_ sender: NSSwitch) {
        Preferences.shared.contentIndexing = sender.state == .on
        index.contentPreferenceChanged()
        refresh()
    }

    @objc private func loginChanged(_ sender: NSSwitch) {
        do {
            if sender.state == .on { try SMAppService.mainApp.register() }
            else { try SMAppService.mainApp.unregister() }
        } catch {
            sender.state = SMAppService.mainApp.status == .enabled ? .on : .off
            footerLabel.stringValue = "Could not update Launch at Login: \(error.localizedDescription)"
        }
    }

    @objc private func rescan() {
        index.rebuild()
        indexSubtitle.stringValue = "Rescanning…"
    }

    @objc private func contacts() {
        providers.requestContactsAccess { [weak self] _ in
            self?.refresh()
        }
    }

    @objc private func diskAccess() {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles")!)
    }

    @objc private func openShortcuts() {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.keyboard?Shortcuts")!)
    }

    @objc private func openStorage() {
        NSWorkspace.shared.open(FileIndex.storageURL)
    }

    @objc private func addExclusion() {
        let picker = NSOpenPanel()
        picker.canChooseFiles = false
        picker.canChooseDirectories = true
        picker.allowsMultipleSelection = true
        picker.prompt = "Exclude"
        guard let window else { return }
        picker.beginSheetModal(for: window) { [weak self] response in
            guard response == .OK else { return }
            Preferences.shared.excludedPaths = Array(Set(Preferences.shared.excludedPaths + picker.urls.map(\.path))).sorted()
            self?.index.exclusionsChanged()
            self?.refresh()
        }
    }

    @objc private func clearExclusions() {
        Preferences.shared.excludedPaths = []
        index.rebuild()
        refresh()
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }
}

private final class SettingsCard: NSView {
    init(rows: [NSView], extraViews: [NSView] = []) {
        super.init(frame: .zero)
        wantsLayer = true
        translatesAutoresizingMaskIntoConstraints = false
        layer?.cornerRadius = 10
        layer?.cornerCurve = .continuous

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 0
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)

        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor)
        ])

        for (index, row) in rows.enumerated() {
            if index > 0 {
                let divider = SettingsDivider()
                stack.addArrangedSubview(divider)
                divider.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
            }
            stack.addArrangedSubview(row)
            row.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        }
        for view in extraViews {
            stack.addArrangedSubview(view)
            view.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        }
        updateColors()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        updateColors()
    }

    private func updateColors() {
        effectiveAppearance.performAsCurrentDrawingAppearance {
            let dark = effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
            layer?.backgroundColor = (dark ? NSColor.white.withAlphaComponent(0.055) : NSColor.black.withAlphaComponent(0.035)).cgColor
        }
    }
}

private final class SettingsDivider: NSView {
    private let line = NSView()

    init() {
        super.init(frame: .zero)
        translatesAutoresizingMaskIntoConstraints = false
        line.wantsLayer = true
        line.translatesAutoresizingMaskIntoConstraints = false
        addSubview(line)
        NSLayoutConstraint.activate([
            heightAnchor.constraint(equalToConstant: 1),
            line.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            line.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            line.topAnchor.constraint(equalTo: topAnchor),
            line.bottomAnchor.constraint(equalTo: bottomAnchor)
        ])
        updateColor()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        updateColor()
    }

    private func updateColor() {
        effectiveAppearance.performAsCurrentDrawingAppearance {
            line.layer?.backgroundColor = NSColor.separatorColor.withAlphaComponent(0.16).cgColor
        }
    }
}
