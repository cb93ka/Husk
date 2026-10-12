// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Whether a game will run here, said as the APK is added and kept on its tile and page: from what it was built with (the scan
/// of its libraries), and, once it has been played, from how that went.
struct Compatibility {
    enum Kind { case plays, likely, unsure, unsupported }
    let kind: Kind
    /// A few words, for a tile ("Not supported").
    let short: String
    /// A line for the game's page.
    let title: String
    /// Why, in a sentence or two.
    let detail: String

    var symbol: String {
        switch kind {
        case .plays: return "checkmark.circle.fill"
        case .likely: return "checkmark.circle"
        case .unsure: return "questionmark.circle"
        case .unsupported: return "xmark.octagon.fill"
        }
    }
    var color: Color {
        switch kind {
        case .plays: return .green
        case .likely: return .blue
        case .unsure: return .orange
        case .unsupported: return .red
        }
    }
}

extension TLReport {
    /// An engine Husk has no translation layer for, told by a library it always ships. These APKs carry Java code too, so without
    /// this they would be tried on the Java runtime and fail with "could not start".
    var unsupportedEngine: String? {
        let names = Set(libraries.filter { $0.abi == "arm64-v8a" }.map(\.name))
        if names.contains("libdmengine.so") { return "Defold" }
        if names.contains("libcocos.so") { return L("Cocos Creator 3") }
        if names.contains("libcocos2djs.so") { return L("Cocos Creator 2 (JavaScript)") }
        if names.contains("libsolar2d.so") || names.contains("libcorona.so") { return "Solar2D" }
        return nil
    }
}

extension TLApp {
    @MainActor var compatibility: Compatibility {
        let last = GameStatusStore.shared.status(id)?.result
        if last == .plays {
            return Compatibility(kind: .plays, short: "Plays", title: L("Plays on this iPhone"),
                                 detail: L("Husk has seen \(label) running here."))
        }
        guard let r = report, r.ok else {
            return Compatibility(kind: .unsupported, short: "Unreadable", title: L("Husk could not read this APK"),
                                 detail: report?.error ?? L("The file may be damaged or not an Android app."))
        }
        if r.webKind != nil {
            return Compatibility(kind: .likely, short: "", title: L("Should run"), detail: L("A web app: its pages run in Safari's engine."))
        }
        if !r.abis.isEmpty && !r.abis.contains("arm64-v8a") {
            let which = r.abis.joined(separator: ", ")
            return Compatibility(kind: .unsupported, short: "Not supported", title: L("Not supported: no 64-bit ARM code"),
                                 detail: L("This APK only has code for \(which). Husk runs 64-bit ARM (arm64-v8a) games; look for an arm64 or universal version of the APK."))
        }
        if let engine = r.unsupportedEngine {
            return Compatibility(kind: .unsupported, short: "Not supported", title: L("Not supported yet: \(engine)"),
                                 detail: L("\(label) is made with \(engine), which Husk has no translation layer for yet, so it will not start."))
        }
        let failedBefore = last == .failed || last == .crashed
        // Qt runs on Husk's Java runtime: its own Java starts the engine, which draws through native EGL under the app's views.
        if r.libraries.contains(where: { $0.abi == "arm64-v8a" && ($0.name.hasPrefix("libQt6Core") || $0.name.hasPrefix("libQt5Core")) }) {
            return Compatibility(kind: failedBefore ? .unsure : .likely, short: failedBefore ? (last?.label ?? "") : "",
                                 title: failedBefore ? (last?.label ?? L("May not run")) : L("Should run: Qt"),
                                 detail: L("A Qt app: Husk runs its Java side on its own runtime and the Qt engine natively. The first start takes a little longer."))
        }
        switch r.nativeEngine {
        case nil:
            return Compatibility(kind: .unsupported, short: "Not supported", title: L("Not supported"),
                                 detail: L("Husk found nothing in this APK it knows how to run."))
        case .java?:
            return Compatibility(kind: .unsure, short: failedBefore ? (last?.label ?? "") : "", title: failedBefore ? (last?.label ?? L("May not run")) : L("May run"),
                                 detail: L("An Android app\(r.engine.map { " (\($0))" } ?? ""): Husk runs its Java code on its own runtime. Many such apps work; some need parts of Android Husk does not have."))
        case .some:
            let name = r.nativeEngineName
            if failedBefore {
                return Compatibility(kind: .unsure, short: last?.label ?? "", title: last?.label ?? L("May not run"),
                                     detail: L("\(name) is supported, but \(label) did not get going last time. Husk's log says why."))
            }
            return Compatibility(kind: .likely, short: "", title: L("Should run: \(name)"),
                                 detail: L("\(name) is one of the engines Husk runs natively."))
        }
    }
}

/// The verdict as a row on a game's page.
struct CompatibilityRow: View {
    let compat: Compatibility

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: compat.symbol)
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(compat.color)
            VStack(alignment: .leading, spacing: 3) {
                Text(Lv(compat.title)).font(.subheadline.weight(.semibold))
                Text(Lv(compat.detail)).font(.footnote).foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 4)
    }
}
