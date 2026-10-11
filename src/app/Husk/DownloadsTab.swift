// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Downloads from a link, and what has an update.
struct DownloadsTab: View {
    @ObservedObject private var downloads = Downloads.shared
    @ObservedObject private var updates = AppUpdates.shared
    @ObservedObject private var guest = GuestImage.shared
    @State private var adding = false
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                updatesSection
                Section {
                    if downloads.jobs.isEmpty {
                        VStack(alignment: .leading, spacing: 6) {
                            Text(LocalizedStringKey(L("No downloads"))).font(.headline)
                            Text(LocalizedStringKey(L("Add a link to an APK or a file, or the address a computer gives you when it shares a folder.")))
                                .font(.subheadline).foregroundStyle(.secondary)
                        }
                        .padding(.vertical, 6)
                    }
                    ForEach(downloads.jobs) { job in
                        DownloadRow(job: job, speed: downloads.speed)
                            .swipeActions {
                                Button(role: .destructive) { downloads.remove(job.id) } label: { Label(LocalizedStringKey(L("Remove")), systemImage: "trash") }
                            }
                    }
                } header: {
                    HStack {
                        Text(LocalizedStringKey(L("Downloads")))
                        Spacer()
                        if downloads.jobs.contains(where: \.complete) {
                            Button(LocalizedStringKey(L("Clear Finished"))) { downloads.clearFinished() }.font(.caption).textCase(nil)
                        }
                    }
                } footer: {
                    VStack(alignment: .leading, spacing: 6) {
                        if downloads.liveActivitiesOff {
                            Text(LocalizedStringKey(L("Live Activities are off for Husk, so downloads don't show on the Lock Screen or in the Dynamic Island. Turn them on in Settings › Husk › Live Activities.")))
                                .foregroundStyle(.orange)
                        }
                        if let free = Downloads.freeSpace() { Text(LocalizedStringKey(L("\(Downloads.bytes(free)) free on this device."))) }
                    }
                }
            }
            .navigationTitle(LocalizedStringKey(L("Downloads")))
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button(LocalizedStringKey(L("Done"))) { dismiss() } }
                ToolbarItem(placement: .primaryAction) {
                    Button { adding = true } label: { Label(LocalizedStringKey(L("Add")), systemImage: "plus") }
                }
            }
            .sheet(isPresented: $adding) { AddDownloadSheet() }
        }
    }

    @ViewBuilder private var updatesSection: some View {
        Section(LocalizedStringKey(L("Updates"))) {
            if let release = updates.available {
                HStack {
                    Label(LocalizedStringKey(L("Husk \(release.version)")), systemImage: "arrow.down.app.fill")
                    Spacer()
                    Button(LocalizedStringKey(L("View"))) { UIApplication.shared.open(release.page) }.buttonStyle(.bordered)
                }
            }
            if guest.update.isSomething {
                HStack {
                    Label(Lv(guest.update.title), systemImage: "shippingbox.fill")
                    Spacer()
                    Button(LocalizedStringKey(L("Download"))) { guest.applyUpdate() }.buttonStyle(.bordered)
                }
            }
            if updates.available == nil && !guest.update.isSomething {
                Label(LocalizedStringKey(L("Everything is up to date")), systemImage: "checkmark.circle").foregroundStyle(.secondary)
            }
        }
    }
}

private struct DownloadRow: View {
    let job: Downloads.Job
    let speed: Double

    private var fraction: Double { job.total > 0 ? min(1, Double(job.received) / Double(job.total)) : 0 }

    private var status: String {
        if job.complete { return job.kind == .apk ? L("Added to the library") : job.kind == .folder ? L("In Shared Storage") : L("In Shared Storage") }
        if job.paused { return L("Paused · \(Downloads.bytes(job.received)) of \(Downloads.bytes(job.total))") }
        if let err = job.files.first(where: { $0.error != nil })?.error { return L("Stopped: \(err)") }
        var s = L("\(Downloads.bytes(job.received)) of \(Downloads.bytes(job.total))")
        if job.files.count > 1 { s += " · \(job.filesDone)/\(job.files.count) files" }
        if speed > 1 {
            s += " · \(Downloads.bytes(Int64(speed)))/s"
            let left = Double(job.total - job.received) / speed
            if left.isFinite, left > 0 {
                let f = DateComponentsFormatter(); f.allowedUnits = left > 3600 ? [.hour, .minute] : [.minute, .second]; f.unitsStyle = .abbreviated
                if let t = f.string(from: left) { s += " · \(t) left" }
            }
        }
        return s
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 10) {
                Image(systemName: job.kind == .folder ? "folder.fill" : job.kind == .apk ? "app.badge.fill" : "doc.fill")
                    .foregroundStyle(job.complete ? Color.green : Color.accentColor)
                Text(Lv(job.title)).font(.headline).lineLimit(1)
                Spacer()
                if !job.complete {
                    Button {
                        if job.paused || job.failed { Downloads.shared.resume(job.id) } else { Downloads.shared.pause(job.id) }
                    } label: {
                        Image(systemName: job.paused || job.failed ? "arrow.clockwise.circle.fill" : "pause.circle.fill").font(.title2)
                    }
                    .buttonStyle(.borderless)
                }
            }
            if !job.complete { ProgressView(value: fraction) }
            Text(Lv(status)).font(.caption).foregroundStyle(job.failed ? Color.orange : Color.secondary).lineLimit(2)
        }
        .padding(.vertical, 4)
    }
}

private struct AddDownloadSheet: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject private var downloads = Downloads.shared
    @State private var link = ""
    @State private var working = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("https://… or http://192.168.1.20:8642/", text: $link)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                } footer: {
                    if let problem = downloads.problem { Text(Lv(problem)).foregroundStyle(.orange) }
                }
                Section {
                    Text(LocalizedStringKey(L("An APK is added to your library when it finishes. Anything else goes into the Shared Storage folder, which games see as their phone storage.")))
                    Text(LocalizedStringKey(L("To send a whole folder from a computer on the same Wi‑Fi, run this on it and enter the address it shows:")))
                    Text(LocalizedStringKey(L("python3 serve-folder.py <folder>")))
                        .font(.system(.footnote, design: .monospaced))
                        .textSelection(.enabled)
                    Text(LocalizedStringKey(L("serve-folder.py is in Husk's repository, under tools. The folder's contents land at the top of Shared Storage, so serve the folder that holds what a game expects there.")))
                } header: {
                    Text(LocalizedStringKey(L("How it works")))
                }
                .font(.footnote)
                .foregroundStyle(.secondary)
            }
            .navigationTitle(LocalizedStringKey(L("Add Download")))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button(LocalizedStringKey(L("Cancel"))) { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    if working { ProgressView() } else {
                        Button(LocalizedStringKey(L("Download"))) {
                            working = true
                            Task {
                                await downloads.add(link: link)
                                working = false
                                if downloads.problem == nil { dismiss() }
                            }
                        }
                        .disabled(link.trimmingCharacters(in: .whitespaces).isEmpty)
                    }
                }
            }
            .onAppear { downloads.problem = nil }
        }
    }
}
