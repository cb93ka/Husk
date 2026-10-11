// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// What covers a game's screen until its first frames are up: the game's icon and name, what the start is busy with
/// ("Loading libunity.so", "Unpacking the game's data… 43%") and a way out. A start can take a minute -- a .NET runtime warming
/// up, a game unpacking a gigabyte on its first run -- and a black screen all that time looked like a hang.
///
/// When the start fails, or the game ends before it ever drew, the same place says so instead of leaving the screen black.
struct LaunchCurtain: View {
    let app: TLApp
    @ObservedObject var model: TLUnityModel
    let onClose: () -> Void

    private var failed: Bool {
        model.state == Int32(HUSK_UNITY_FAILED) || model.state == Int32(HUSK_UNITY_ENDED)
    }

    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(white: 0.10), Color(white: 0.03)], startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea()
            VStack(spacing: 14) {
                ItemIcon(item: .game(app), size: 84)
                    .shadow(color: .black.opacity(0.5), radius: 14, y: 6)
                Text(Lv(app.label))
                    .font(.system(size: 20, weight: .semibold))
                    .foregroundStyle(.white)
                    .lineLimit(2).multilineTextAlignment(.center)
                if failed {
                    failure
                } else {
                    progress
                }
            }
            .frame(maxWidth: 420)
            .padding(24)
            VStack {
                HStack {
                    Button(action: onClose) {
                        Label(failed ? "Close" : "Cancel", systemImage: "xmark")
                            .font(.system(size: 14, weight: .semibold))
                            .padding(.horizontal, 14).padding(.vertical, 8)
                            .background(.white.opacity(0.12), in: Capsule())
                    }
                    .tint(.white)
                    Spacer()
                }
                Spacer()
            }
            .padding(16)
        }
    }

    @ViewBuilder private var progress: some View {
        VStack(spacing: 10) {
            if model.fraction >= 0 {
                ProgressView(value: min(max(model.fraction, 0), 1))
                    .progressViewStyle(.linear)
                    .tint(.white)
                    .frame(width: 240)
            } else {
                ProgressView().progressViewStyle(.circular).tint(.white)
            }
            Text(Lv(stageText))
                .font(.system(size: 13, weight: .medium))
                .foregroundStyle(.white.opacity(0.75))
                .lineLimit(2).multilineTextAlignment(.center)
                .animation(nil, value: stageText)
        }
        .padding(.top, 6)
    }

    private var stageText: String {
        let stage = model.stage.isEmpty ? L("Starting…") : model.stage
        guard model.fraction >= 0 else { return stage }
        return "\(stage) — \(Int((model.fraction * 100).rounded()))%"
    }

    private var failure: some View {
        VStack(spacing: 8) {
            Label(model.state == Int32(HUSK_UNITY_FAILED) ? L("Could not start") : L("The game closed before it started"),
                  systemImage: "exclamationmark.triangle.fill")
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(.orange)
            Text(LocalizedStringKey(L("Husk's log (husk.log in the Husk folder in Files) says why. Close Husk completely before trying again: a game cannot be started twice in one run.")))
                .font(.system(size: 13))
                .foregroundStyle(.white.opacity(0.7))
                .multilineTextAlignment(.center)
        }
        .padding(.top, 6)
    }
}
