// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Settings, as the iOS Settings app lays them out: a card for the app up top, then groups of rows that each
/// push a page of their own. What belongs to the app, what belongs to the emulator, and what the thing is.
struct SettingsTab: View {
    @AppStorage(AppWindows.defaultKey) private var multitask = false
    @AppStorage(CompatibilityStore.askKey) private var askCompat = true

    var body: some View {
        NavigationStack {
            List {
                Section {
                    NavigationLink { AboutSettings() } label: { appCard }
                }

                Section {
                    row(JITSettings(), "bolt.fill", .yellow, "JIT & Sideload", "StikJIT is built in — the recommended way")
                } header: {
                    Text(LocalizedStringKey(L("JIT")))
                } footer: {
                    Text(LocalizedStringKey(L("Android and Translation Layer games need JIT. StikJIT, built into Husk, turns it on without a computer.")))
                }

                Section(LocalizedStringKey(L("General"))) {
                    row(DiscoverView(), "sparkle.magnifyingglass", .mint, "Discover", "Find apps in F-Droid and other repositories")
                    row(LibrarySettings(), "square.grid.2x2.fill", .blue, "Library", "Your apps and their icons")
                    row(PerformanceSettings(), "speedometer", .orange, "Performance", "Renderer, sound")
                    row(AppearanceSettings(), "paintbrush.fill", .pink, "Appearance", "Light or dark, accent colour, app icon")
                }

                Section {
                    Toggle(isOn: $multitask) {
                        Label {
                            VStack(alignment: .leading, spacing: 1) {
                                Text(LocalizedStringKey(L("Open Apps in Windows")))
                                Text(LocalizedStringKey(L("Use Husk while an app runs"))).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                            }
                        } icon: {
                            RoundedRectangle(cornerRadius: 7, style: .continuous)
                                .fill(Color.purple.gradient)
                                .frame(width: 30, height: 30)
                                .overlay {
                                    Image(systemName: "macwindow.on.rectangle")
                                        .font(.system(size: 14, weight: .medium))
                                        .foregroundStyle(.white)
                                }
                        }
                    }
                } header: {
                    Text(LocalizedStringKey(L("Multitasking")))
                } footer: {
                    Text(LocalizedStringKey(L("Apps and games open in a window you can move, resize and minimise to a dock, like LiveContainer's multitasking. Each app can override this in its own settings.")))
                }

                Section {
                    Toggle(isOn: $askCompat) {
                        Label {
                            VStack(alignment: .leading, spacing: 1) {
                                Text("Ask How Games Ran")
                                Text("After playing, once per game per update").font(.caption).foregroundStyle(.secondary).lineLimit(1)
                            }
                        } icon: {
                            RoundedRectangle(cornerRadius: 7, style: .continuous)
                                .fill(Color.green.gradient)
                                .frame(width: 30, height: 30)
                                .overlay {
                                    Image(systemName: "list.bullet.clipboard")
                                        .font(.system(size: 14, weight: .medium))
                                        .foregroundStyle(.white)
                                }
                        }
                    }
                    Link(destination: CompatibilityStore.site) {
                        Label("Compatibility List", systemImage: "safari")
                    }
                } header: {
                    Text("Compatibility")
                } footer: {
                    Text("Game pages and the Store show how each app runs for everyone, from antiweb.lol. Your reports go there "
                       + "with your device, iOS version, Husk build and JIT method.")
                }

                Section(LocalizedStringKey(L("Emulator"))) {
                    row(InputSettings(), "hand.tap.fill", .indigo, "Input", "Screen, touch, keyboard")
                    row(NetworkSettings(), "globe", .green, "Network", "Internet and saved sessions")
                    row(SavedMachineSettings(), "externaldrive.fill", .teal, "Saved Machine", "Snapshots and automatic saving")
                }
            }
            .listStyle(.insetGrouped)
            .navigationTitle(LocalizedStringKey(L("Settings")))
        }
    }

    private var appCard: some View {
        HStack(spacing: 14) {
            HuskMark(size: 56)
            VStack(alignment: .leading, spacing: 2) {
                Text("Husk").font(.title3.weight(.semibold))
                Text(LocalizedStringKey(L("Version \(Bundle.main.version) · \(Bundle.main.commit)")))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 6)
    }

    private func row<D: View>(_ destination: D, _ icon: String, _ tint: Color,
                              _ title: String, _ subtitle: String) -> some View {
        NavigationLink { destination } label: {
            Label {
                VStack(alignment: .leading, spacing: 1) {
                    Text(Lv(title))
                    Text(Lv(subtitle)).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
            } icon: {
                RoundedRectangle(cornerRadius: 7, style: .continuous)
                    .fill(tint.gradient)
                    .frame(width: 30, height: 30)
                    .overlay {
                        Image(systemName: icon)
                            .font(.system(size: 15, weight: .medium))
                            .foregroundStyle(.white)
                    }
            }
        }
    }
}

/// A Form, as the system draws one.
private struct HuskForm: ViewModifier {
    func body(content: Content) -> some View {
        content.navigationBarTitleDisplayMode(.inline)
    }
}

extension View {
    func huskForm() -> some View { modifier(HuskForm()) }
}

// MARK: - Library

struct LibrarySettings: View {
    @ObservedObject private var host = AndroidHost.shared
    @State private var working = false

    var body: some View {
        Form {
            Section {
                DetailRow(label: L("Apps"), value: "\(host.packages.count)", mono: false)
                DetailRow(label: L("With icons"),
                          value: "\(host.packages.filter { $0.iconPath != nil }.count)",
                          mono: false)
            } footer: {
                Text(LocalizedStringKey(L("The list is written to disk, so it is on screen before Android has finished starting.")))
            }

            Section {
                Button {
                    working = true
                    Task { await host.refreshPackages(); working = false }
                } label: {
                    Label(working ? L("Refreshing…") : L("Refresh from Android"),
                          systemImage: "arrow.clockwise")
                }
                .disabled(working || !host.isReady)

                Button {
                    AndroidHost.forgetIcons()
                    working = true
                    Task { await host.refreshPackages(); working = false }
                } label: {
                    Label(LocalizedStringKey(L("Re-fetch icons")), systemImage: "photo.on.rectangle")
                }
                .disabled(working || !host.isReady)
            } footer: {
                Text(LocalizedStringKey(L("Names and icons come from Android's own launcher, which keeps the version it draws. Re-fetching throws away Husk's copies and asks again.")))
            }
        }
        .huskForm()
        .navigationTitle(LocalizedStringKey(L("Library")))
    }
}

// MARK: - Performance

struct PerformanceSettings: View {
    @ObservedObject private var runner = QemuRunner.shared
    @State private var gpuMode =
        UserDefaults.standard.object(forKey: "husk.gpuMode") as? Bool ?? true
    @State private var sound = UserDefaults.standard.bool(forKey: "husk.sound")
    @State private var soundDevice =
        UserDefaults.standard.object(forKey: "husk.soundDevice") as? Bool ?? true

    var body: some View {
        Form {
            Section {
                Picker(LocalizedStringKey(L("Renderer")), selection: $gpuMode) {
                    Text(LocalizedStringKey(L("GPU"))).tag(true)
                    Text(LocalizedStringKey(L("CPU"))).tag(false)
                }
                .pickerStyle(.segmented)
                .onChange(of: gpuMode) { v in
                    UserDefaults.standard.set(v, forKey: "husk.gpuMode")
                    HuskLog.log("ui", v ? "GPU renderer selected" : "CPU renderer selected")
                }
            } header: {
                Text(LocalizedStringKey(L("Renderer")))
            } footer: {
                Text(gpuMode
                     ? L("Android draws on the real GPU through Metal — about four times ")
                     + "the frame rate. This is the default."
                     : L("Every pixel is drawn by the emulated CPU. Much slower, and only ")
                     + "worth choosing if the GPU misbehaves.")
            }

            Section {
                DetailRow(label: L("Frame rate"),
                          value: runner.fps > 0
                                 ? String(format: "%.0f fps", runner.fps) : "—")
                DetailRow(label: L("Guest screen"),
                          value: "\(QemuRunner.lastGuestRes.w)×\(QemuRunner.lastGuestRes.h)")
            } header: {
                Text(LocalizedStringKey(L("Now")))
            }

            Section {
                Toggle(LocalizedStringKey(L("Sound")), isOn: $sound)
                    .onChange(of: sound) { v in
                        UserDefaults.standard.set(v, forKey: "husk.sound")
                        HuskLog.log("ui", v ? "sound on" : "sound off")
                    }
                if sound {
                    Toggle(LocalizedStringKey(L("Attach the sound device")), isOn: $soundDevice)
                        .onChange(of: soundDevice) { v in
                            UserDefaults.standard.set(v, forKey: "husk.soundDevice")
                        }
                }
            } header: {
                Text(LocalizedStringKey(L("Sound")))
            } footer: {
                Text(LocalizedStringKey(L("Adds a sound device. While it is attached Android cannot be saved — QEMU refuses to snapshot a machine with one — so every launch boots from cold. Turning it on or off costs a cold boot either way.")))
            }
        }
        .huskForm()
        .navigationTitle(LocalizedStringKey(L("Performance")))
    }
}

// MARK: - Input

struct InputSettings: View {
    @State private var landscapeGuest =
        UserDefaults.standard.bool(forKey: "husk.landscapeGuest")
    @State private var customRes = UserDefaults.standard.bool(forKey: "husk.customRes")
    @State private var widthText = InputSettings.stored("husk.resWidth", 720)
    @State private var heightText = InputSettings.stored("husk.resHeight", 1280)

    /// Sizes worth offering without typing. Deliberately short: these are the
    /// shapes a phone guest is actually run at, not a catalogue of every panel
    /// ever made.
    private static let presets: [(name: String, w: Int, h: Int)] = [
        ("Small — 360 × 800", 360, 800),
        ("HD — 720 × 1280", 720, 1280),
        ("Full HD — 1080 × 1920", 1080, 1920),
        ("Landscape HD — 1280 × 720", 1280, 720),
        ("Tablet — 1280 × 800", 1280, 800),
    ]

    private static func stored(_ key: String, _ fallback: Int) -> String {
        let v = UserDefaults.standard.integer(forKey: key)
        return String(v > 0 ? v : fallback)
    }

    /// What the typed numbers actually come to, or nothing if they are not a
    /// size the guest can be given.
    private var effective: (w: Int, h: Int)? {
        QemuRunner.validResolution(w: Int(widthText) ?? 0, h: Int(heightText) ?? 0)
    }

    var body: some View {
        Form {
            Section {
                Picker(LocalizedStringKey(L("Screen")), selection: $landscapeGuest) {
                    Text(LocalizedStringKey(L("Portrait"))).tag(false)
                    Text(LocalizedStringKey(L("Landscape"))).tag(true)
                }
                .pickerStyle(.segmented)
                .disabled(customRes)
                .onChange(of: landscapeGuest) { v in
                    UserDefaults.standard.set(v, forKey: "husk.landscapeGuest")
                    HuskLog.log("ui", v ? "guest panel will be landscape"
                                        : "guest panel will be portrait")
                }
            } header: {
                Text(LocalizedStringKey(L("Screen")))
            } footer: {
                Text(customRes
                     ? L("A custom resolution sets the shape itself, so this does nothing ")
                     + "while it is on. Type a wide size for landscape."
                     : L("Android cannot reshape a screen once it is running, so a ")
                     + "landscape game on a portrait screen gets letterboxed into a "
                     + "band and looks tiny. Creating it landscape is the only way it "
                     + "can fill it — portrait apps are letterboxed instead. Costs one "
                     + "cold boot.")
            }

            Section {
                Toggle(LocalizedStringKey(L("Custom resolution")), isOn: $customRes)
                    .onChange(of: customRes) { v in
                        UserDefaults.standard.set(v, forKey: "husk.customRes")
                        store()
                        HuskLog.log("ui", v ? "custom resolution on: "
                                            + "\(widthText)x\(heightText)"
                                            : "custom resolution off")
                    }

                if customRes {
                    Picker(LocalizedStringKey(L("Preset")), selection: Binding(
                        get: { presetIndex },
                        set: { i in
                            guard i >= 0, i < Self.presets.count else { return }
                            widthText = String(Self.presets[i].w)
                            heightText = String(Self.presets[i].h)
                            store()
                        })) {
                        ForEach(0..<Self.presets.count, id: \.self) { i in
                            Text(Self.presets[i].name).tag(i)
                        }
                        Text(LocalizedStringKey(L("Custom"))).tag(-1)
                    }

                    HStack {
                        Text(LocalizedStringKey(L("Width")))
                        Spacer()
                        TextField("720", text: $widthText)
                            .keyboardType(.numberPad)
                            .multilineTextAlignment(.trailing)
                            .font(.technical())
                            .frame(width: 90)
                            .onChange(of: widthText) { _ in store() }
                    }
                    HStack {
                        Text(LocalizedStringKey(L("Height")))
                        Spacer()
                        TextField("1280", text: $heightText)
                            .keyboardType(.numberPad)
                            .multilineTextAlignment(.trailing)
                            .font(.technical())
                            .frame(width: 90)
                            .onChange(of: heightText) { _ in store() }
                    }

                    if let size = effective {
                        DetailRow(label: L("Android will get"),
                                  value: "\(size.w) × \(size.h)")
                    } else {
                        Text(LocalizedStringKey(L("Both sides must be between 240 and 2560.")))
                            .font(.caption).foregroundStyle(.orange)
                    }
                }

                DetailRow(label: L("Running now"), value: running)
            } header: {
                Text(LocalizedStringKey(L("Resolution")))
            } footer: {
                Text(LocalizedStringKey(L("The panel is built when the machine starts, so a change costs one cold boot, and the next save replaces the machine saved at the old size — changing back costs another. Sizes are rounded to a multiple of eight. Bigger is slower: every pixel is drawn by an emulated phone. Android's density does not change with the panel, so a larger one shows more rather than bigger.")))
            }

            Section {
                Text(LocalizedStringKey(L("Touch is always on. The keyboard and the rotate control are in the pill at the bottom of the guest's screen; a gamepad and a pointer are not wired through yet.")))
                    .font(.footnote).foregroundStyle(.secondary)
            } header: {
                Text(LocalizedStringKey(L("Controls")))
            }
        }
        .huskForm()
        .navigationTitle(LocalizedStringKey(L("Input")))
    }

    /// The panel the guest actually has, which only means anything while there
    /// is a guest: the stored value is last launch's until one starts.
    private var running: String {
        guard QemuRunner.shared.isRunning else { return "not started" }
        return "\(QemuRunner.lastGuestRes.w) × \(QemuRunner.lastGuestRes.h)"
    }

    /// Which preset the typed numbers are, if any.
    private var presetIndex: Int {
        guard let size = effective else { return -1 }
        return Self.presets.firstIndex { $0.w == size.w && $0.h == size.h } ?? -1
    }

    private func store() {
        UserDefaults.standard.set(Int(widthText) ?? 0, forKey: "husk.resWidth")
        UserDefaults.standard.set(Int(heightText) ?? 0, forKey: "husk.resHeight")
    }
}

// MARK: - Network

struct NetworkSettings: View {
    @State private var keepNetwork =
        UserDefaults.standard.object(forKey: "husk.keepNetwork") as? Bool ?? true

    var body: some View {
        Form {
            Section {
                Toggle(LocalizedStringKey(L("Keep the network across saves")), isOn: $keepNetwork)
                    .onChange(of: keepNetwork) { v in
                        UserDefaults.standard.set(v, forKey: "husk.keepNetwork")
                    }
            } footer: {
                Text(keepNetwork
                     ? L("Saving closes apps but leaves Android's framework running, so ")
                     + "the network still works after a restore."
                     : L("Saving stops the framework too. Clears every GPU resource, ")
                     + "which is steadier — but the network may not come back until a "
                     + "cold boot.")
            }

            Section {
                Text(LocalizedStringKey(L("Android reaches the internet through a virtual ethernet card on QEMU's own network. Nothing on your phone's network can see the guest, and the guest cannot see it.")))
                    .font(.footnote).foregroundStyle(.secondary)
            } header: {
                Text(LocalizedStringKey(L("How it connects")))
            }
        }
        .huskForm()
        .navigationTitle(LocalizedStringKey(L("Network")))
    }
}

// MARK: - JIT and sideloading

struct JITSettings: View {
    @ObservedObject private var runner = QemuRunner.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @State private var autoStart = Onboarding.autoStart
    @AppStorage(JITCoordinator.autoEnableKey) private var autoJIT = false
    @State private var keepAttached = JITBootstrap.keepDebuggerAttached

    private var pairingLabel: String {
        switch jit.pairingSource {
        case .onDevice: return "paired on this device"
        case .imported: return "file imported"
        case nil: return "not set up"
        }
    }

    var body: some View {
        Form {
            Section {
                JITCard()
            }
            .listRowInsets(EdgeInsets())
            .listRowBackground(Color.clear)

            Section {
                Picker(LocalizedStringKey(L("Method")), selection: $jit.method) {
                    ForEach(JITMethod.allCases) { Text($0.title).tag($0) }
                }
                DetailRow(label: L("StikDebug"),
                          value: JITBootstrap.isStikDebugInstalled ? "installed" : "not found", mono: false)
                DetailRow(label: L("TrollStore"),
                          value: JITBootstrap.isTrollStoreInstalled ? "installed" : "not found", mono: false)
                DetailRow(label: L("Installed with TrollStore"),
                          value: JITBootstrap.isInstalledWithTrollStore ? "yes" : "no", mono: false)
                DetailRow(label: L("Jailbreak"),
                          value: JITBootstrap.debuggedAtLaunch ? L("JIT allowed for apps")
                               : JITBootstrap.isJailbroken ? "found" : "not found", mono: false)
                DetailRow(label: L("Built-in pairing"), value: pairingLabel, mono: false)
                Button {
                    jit.showSetup = true
                } label: {
                    Label(LocalizedStringKey(L("Set up JIT")), systemImage: "wand.and.stars")
                }
            } header: {
                Text(LocalizedStringKey(L("Other Ways to Turn On JIT")))
            } footer: {
                Text(jit.method == .automatic
                     ? jit.automaticDescription + " Built-in StikJIT needs iOS 26, LocalDevVPN, and a "
                       + "pairing file, which Husk can make itself on iOS 27."
                     : HuskBuiltInJIT.unavailableReason ?? L("Built-in StikJIT needs LocalDevVPN and a pairing ")
                       + "file, which Husk can make itself on iOS 27.")
            }

            Section {
                DetailRow(label: L("Debugger"),
                          value: JITBootstrap.isDebuggerAttached ? "attached" : "not attached",
                          mono: false)
                DetailRow(label: L("Executable memory"),
                          value: JITBootstrap.isLive ? "granted" : "not claimed", mono: false)
                // The two routes, named separately. Either one is enough, and
                // when someone reports "JIT does not work" these two rows are
                // the whole diagnosis.
                DetailRow(label: L("Trap servicer"),
                          value: husk_ios_jit_self_route() != nil ? "not needed"
                               : JITBootstrap.prewarmed ? "answering" : "not answering",
                          mono: false)
                if let route = husk_ios_jit_self_route() {
                    DetailRow(label: L("Made by Husk"),
                              value: String(cString: route) == "MAP_JIT" ? L("MAP_JIT (TrollStore entitlement)") : "plain memory (debugged or jailbroken)",
                              mono: false)
                }
                // Cached answer only: running the probe from a view body
                // could freeze the app (see JITBootstrap.mapJITWorks).
                DetailRow(label: L("MAP_JIT"),
                          value: JITBootstrap.deviceEnforcesTXM ? "not used (TXM)"
                               : JITBootstrap.mapJITResult.map { $0 ? "executes" : "refused" }
                                 ?? "not tested",
                          mono: false)
                DetailRow(label: L("Debugger after setup"),
                          value: JITBootstrap.detached ? "detached" : "attached",
                          mono: false)
                if let why = JITBootstrap.lastFailure {
                    Text(Lv(why)).font(.caption).foregroundStyle(.orange)
                }
                if !JITBootstrap.ready {
                    Button {
                        jit.enable()
                    } label: {
                        Label(LocalizedStringKey(L("Enable JIT with \(jit.resolvedMethod.title)")), systemImage: "bolt.fill")
                    }
                    .disabled(jit.busy)
                    Button {
                        _ = JITBootstrap.requestTrollStoreAttach()
                    } label: {
                        Label(LocalizedStringKey(L("Enable JIT with TrollStore")), systemImage: "sparkles")
                    }
                }
            } header: {
                Text(LocalizedStringKey(L("JIT")))
            } footer: {
                Text(LocalizedStringKey(L("Husk needs memory it can write and then execute, which on iOS takes an attached debugger. There are two ways to get it: a debugger that services trap requests, or a MAP_JIT mapping, which the kernel allows any debugged process. Either one is enough — which is available depends on the device and the iOS version, so Husk tests both rather than assuming.")))
            }

            Section {
                Toggle(LocalizedStringKey(L("Turn On JIT at Launch")), isOn: $autoJIT)
                    .disabled(!HuskBuiltInJIT.isAvailable)
            } footer: {
                Text(HuskBuiltInJIT.isAvailable
                     ? L("Each time Husk opens without JIT, it asks the built-in StikJIT to turn it on, so games are ready ")
                       + "without a tap. Needs StikJIT set up once (paired) first."
                     : L("Needs the built-in StikJIT, which is available on iOS 26 and later."))
            }

            Section {
                Toggle(LocalizedStringKey(L("Start Android on launch")), isOn: $autoStart)
                    .onChange(of: autoStart) { v in
                        UserDefaults.standard.set(v, forKey: "husk.autoStart")
                    }
            } footer: {
                Text(LocalizedStringKey(L("Boots the guest as soon as Husk opens, when JIT is available.")))
            }

            Section {
                Toggle(LocalizedStringKey(L("Keep debugger attached")), isOn: $keepAttached)
                    .onChange(of: keepAttached) { v in JITBootstrap.keepDebuggerAttached = v }
            } footer: {
                Text(LocalizedStringKey(L("Off by default. Husk detaches StikDebug as soon as the JIT region is held, because a debugger that iOS has suspended stops the whole app the next time it is needed. Turn this on only to collect StikDebug's own logs.")))
            }

            Section {
                Text(LocalizedStringKey(L("APKs install from the Library's + button or from the Files tab. Split sets — a base APK plus its config pieces — must be picked together; installing the base alone fails on missing native libraries.")))
                    .font(.footnote).foregroundStyle(.secondary)
            } header: {
                Text(LocalizedStringKey(L("Sideloading")))
            }
        }
        .huskForm()
        .navigationTitle(LocalizedStringKey(L("JIT & sideload")))
    }
}

// MARK: - Saved machine

struct SavedMachineSettings: View {
    @ObservedObject private var runner = QemuRunner.shared
    @State private var autoSave =
        UserDefaults.standard.object(forKey: "husk.autoSave") as? Bool ?? true
    @State private var useSnapshot =
        UserDefaults.standard.object(forKey: "husk.downloadSnapshot") as? Bool ?? true
    @State private var askWhichToDelete = false
    @State private var deleteResult: String?
    @AppStorage(GuestImage.askUpdatesKey) private var askUpdates = true

    var body: some View {
        Form {
            Section {
                Toggle(LocalizedStringKey(L("Save automatically")), isOn: $autoSave)
                    .onChange(of: autoSave) { v in
                        UserDefaults.standard.set(v, forKey: "husk.autoSave")
                        HuskLog.log("ui", v ? "automatic saving on" : "automatic saving off")
                    }
                Button {
                    QemuRunner.shared.saveState(reason: "asked from settings")
                } label: {
                    Label(runner.isSavingState ? L("Saving…") : L("Save now"),
                          systemImage: "externaldrive.badge.checkmark")
                }
                .disabled(runner.isSavingState)
            } footer: {
                Text(LocalizedStringKey(L("Husk restores a saved machine instead of booting it, which takes seconds rather than minutes. The picture freezes while it writes. With this off, nothing saves by itself — including after an install.")))
            }

            Section {
                Button(role: .destructive) { askWhichToDelete = true } label: {
                    Label(LocalizedStringKey(L("Delete saved machine")), systemImage: "trash")
                }
                .disabled(!QemuRunner.shared.hasSnapshot)
                if let deleteResult {
                    Text(Lv(deleteResult)).font(.caption).foregroundStyle(.secondary)
                }
            } footer: {
                Text(QemuRunner.shared.hasSnapshot
                     ? L("Currently saved: ")
                     + ((QemuRunner.shared.snapshotDisplay ?? "sw").contains("gl")
                        ? "GPU" : "software") + "."
                     : L("Nothing is saved, so Android boots from cold."))
            }

            Section {
                Toggle(LocalizedStringKey(L("Download pre-booted snapshot")), isOn: $useSnapshot)
                    .onChange(of: useSnapshot) { v in
                        UserDefaults.standard.set(v, forKey: "husk.downloadSnapshot")
                    }
            } footer: {
                Text(LocalizedStringKey(L("Adds about 2 GB to the first download. It was captured on the software renderer, so it is not used on GPU — which cold-boots once and then saves its own.")))
            }

            Section {
                Toggle(LocalizedStringKey(L("Ask about Android updates")), isOn: $askUpdates)
            } footer: {
                Text(LocalizedStringKey(L("When a new Android image or snapshot is published, Husk asks once whether to download it. Never asked before Android has been downloaded.")))
            }
        }
        .huskForm()
        .navigationTitle(LocalizedStringKey(L("Saved machine")))
        .confirmationDialog(LocalizedStringKey(L("Which saved machine?")), isPresented: $askWhichToDelete,
                            titleVisibility: .visible) {
            Button(LocalizedStringKey(L("GPU machine")), role: .destructive) { forget("gl", "GPU") }
            Button(LocalizedStringKey(L("Software machine")), role: .destructive) { forget("sw", "software") }
            Button(LocalizedStringKey(L("Cancel")), role: .cancel) { }
        } message: {
            Text(LocalizedStringKey(L("Android will boot from cold once, then save a new one.")))
        }
    }

    private func forget(_ mode: String, _ name: String) {
        if QemuRunner.shared.forgetSnapshot(mode: mode) {
            deleteResult = L("Deleted the \(name) machine. The next launch boots from cold.")
        } else {
            deleteResult = L("No \(name) machine is saved, so nothing was deleted.")
        }
    }
}

// MARK: - Appearance

struct AppearanceSettings: View {
    @State private var appIcon = HuskAppIcon.current
    @AppStorage(Theme.Appearance.key) private var appearance = Theme.Appearance.system
    @State private var language = HuskLanguage.choice
    @AppStorage(Holiday.key) private var holiday = true
    @ObservedObject private var theme = AppTheme.shared
    @Environment(\.colorScheme) private var scheme
    @State private var custom: Color = AppTheme.shared.accentColor

    private let iconColumns = [GridItem(.adaptive(minimum: 92), spacing: 14)]

    var body: some View {
        Form {
            Section {
                Picker(selection: Binding(get: { language }, set: { language = $0; HuskLanguage.choice = $0 })) {
                    ForEach(HuskLanguage.allCases) { Text($0.title).tag($0) }
                } label: {
                    Label(L("Language"), systemImage: "globe")
                }
            } footer: {
                Text(L("Husk's own screens. Games keep their own languages."))
            }
            Section {
                Picker(LocalizedStringKey(L("Appearance")), selection: $appearance) {
                    ForEach(Theme.Appearance.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                .onChange(of: appearance) { v in
                    HuskLog.log("ui", "appearance: \(v.rawValue)")
                }
                Toggle("Holiday Decorations", isOn: $holiday)
            } header: {
                Text(LocalizedStringKey(L("Theme")))
            } footer: {
                Text(LocalizedStringKey(L("System follows the phone: light by day, dark by night. The guest's own screen stays dark either way — it is a picture of another phone.")))
            }

            Section {
                LazyVGrid(columns: Array(repeating: GridItem(.flexible()), count: 5), spacing: 14) {
                    ForEach(AppTheme.presets) { preset in
                        Button {
                            theme.accentColor = preset.color
                            custom = preset.color
                        } label: {
                            swatch(preset.color, selected: preset.color.themeHex == theme.accentColor.themeHex)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(preset.name)
                    }
                }
                .padding(.vertical, 6)

                ColorPicker("Custom Colour", selection: $custom, supportsOpacity: false)
                    .onChange(of: custom) { theme.accentColor = $0 }
            } header: {
                Text(LocalizedStringKey(L("Accent Colour")))
            } footer: {
                Text(LocalizedStringKey(L("Tints buttons, switches and selected states throughout the app.")))
            }

            Section {
                LazyVGrid(columns: iconColumns, spacing: 14) {
                    ForEach(HuskAppIcon.allCases) { icon in
                        Button {
                            appIcon = icon
                            HuskAppIcon.apply(icon)
                        } label: {
                            VStack(spacing: 8) {
                                if let art = icon.preview(dark: scheme == .dark) {
                                    Image(uiImage: art)
                                        .resizable().scaledToFit()
                                        .frame(width: 60, height: 60)
                                        .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                                        .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous)
                                            .strokeBorder(appIcon == icon ? Color.accentColor : .clear, lineWidth: 3))
                                }
                                Text(Lv(icon.title))
                                    .font(.caption)
                                    .foregroundStyle(appIcon == icon ? Color.accentColor : .primary)
                                    .lineLimit(1)
                            }
                            .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.vertical, 6)
            } header: {
                Text(LocalizedStringKey(L("App Icon")))
            } footer: {
                Text(LocalizedStringKey(L("Automatic follows the system appearance — light, dark and tinted. The others pin one look. iOS shows its own confirmation after a change; that alert cannot be turned off.")))
            }
        }
        .navigationTitle(LocalizedStringKey(L("Appearance")))
        .navigationBarTitleDisplayMode(.inline)
    }

    private func swatch(_ color: Color, selected: Bool) -> some View {
        Circle()
            .fill(color)
            .frame(width: 34, height: 34)
            .overlay(Circle().strokeBorder(Color.primary.opacity(0.9), lineWidth: selected ? 2 : 0).padding(-4))
            .overlay {
                if selected {
                    Image(systemName: "checkmark")
                        .font(.caption.bold())
                        .foregroundStyle(color.isLight ? .black : .white)
                }
            }
            .frame(maxWidth: .infinity)
    }
}

// MARK: - About

struct AboutSettings: View {
    @ObservedObject private var runner = QemuRunner.shared
    @State private var showLogs = false
    @AppStorage(TranslationLayer.devInfoKey) private var devInfo = false

    var body: some View {
        List {
            Section {
                VStack(spacing: 8) {
                    HuskMark(size: 84)
                    Text("Husk").font(.title2.weight(.semibold))
                    Text(LocalizedStringKey(L("Version \(Bundle.main.version)")))
                        .font(.subheadline).foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 10)
                .listRowBackground(Color.clear)
            }

            Section {
                LabeledContent(LocalizedStringKey(L("Build")), value: Bundle.main.commit)
                LabeledContent(LocalizedStringKey(L("Guest image")), value: GuestImage.imageVersion)
                LabeledContent(LocalizedStringKey(L("Renderer")),
                               value: runner.displayKind == .gl ? "GPU"
                                    : runner.displayKind == .software ? "CPU"
                                    : L("Not started"))
            }

            Section {
                Toggle(isOn: $devInfo) {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(LocalizedStringKey(L("Developer Info")))
                        Text(LocalizedStringKey(L("Technical detail in the Android Translation Layer screens: library reports, device checks and run logs.")))
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                if devInfo {
                    NavigationLink { TLChecksView() } label: {
                        Label(LocalizedStringKey(L("Device Checks")), systemImage: "stethoscope")
                    }
                    NavigationLink { MassTestView() } label: {
                        Label("Mass Test", systemImage: "square.stack.3d.down.forward")
                    }
                }
                Button { showLogs = true } label: {
                    Label(LocalizedStringKey(L("Open Console")), systemImage: "terminal")
                }
            } footer: {
                Text(LocalizedStringKey(L("Husk runs unmodified Android APKs in a real Android system on your iPhone. The console shows Husk's live log, the guest's serial output and QEMU's own output — the three files any problem here is diagnosed from.")))
            }
        }
        .navigationTitle(LocalizedStringKey(L("About")))
        .navigationBarTitleDisplayMode(.inline)
        .sheet(isPresented: $showLogs) { LogView() }
    }
}

extension Bundle {
    var version: String {
        (infoDictionary?["CFBundleShortVersionString"] as? String) ?? "?"
    }
    var commit: String {
        (infoDictionary?["HuskBuildCommit"] as? String) ?? "?"
    }
}
