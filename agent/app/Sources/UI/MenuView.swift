// Panel shown from the menu bar icon.
import ServiceManagement
import SwiftUI

struct MenuView: View {
    @ObservedObject var model: AgentModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(verbatim: "DJI Dongle").font(.headline)
            statusSection
            Divider()
            controls
            if let message = model.message {
                Text(verbatim: message.text)
                    .font(.callout)
                    .foregroundStyle(message.isError ? Color.red : Color.secondary)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Divider()
            loginSection
            Divider()
            Button(L("menu.open-log")) { model.openLog() }.buttonStyle(.borderless)
            Button(L("menu.quit")) { NSApplication.shared.terminate(nil) }.buttonStyle(.borderless)
                .keyboardShortcut("q")
        }
        .padding(14)
        .frame(width: 320, alignment: .leading)
        .onAppear { model.refreshLoginStatus() }
    }

    @ViewBuilder private var statusSection: some View {
        switch model.status {
        case .missing: problem(L("problem.not-running"))
        case .unreadable: problem(L("problem.unreadable"))
        case .invalid: problem(L("problem.invalid"))
        case .ok(let s):
            VStack(alignment: .leading, spacing: 6) {
                Label { Text(verbatim: stateLine(s, now: model.now)) } icon: {
                    Image(systemName: menuBarSymbol(model.status, now: model.now))
                }
                .font(.body.weight(.medium))
                if isStale(s, now: model.now) {
                    Text(verbatim: L("state.stale")).font(.callout).foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
                row(L("label.signal-at-connect"), signalText(s))
                if s.quality != nil {
                    Text(verbatim: signalValues(s)).font(.caption).foregroundStyle(.secondary).padding(.leading, 2)
                }
                row(L("label.live-rate"), rateText)
                if s.endResult != .none { row(L("label.last-end"), lastEndText(s)) }
            }
        }
    }

    private func problem(_ text: String) -> some View {
        Label { Text(verbatim: text) } icon: { Image(systemName: menuBarSymbol(model.status, now: model.now)) }
            .fixedSize(horizontal: false, vertical: true)
    }

    private func row(_ title: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text(verbatim: title).foregroundStyle(.secondary)
            Text(verbatim: value).monospacedDigit().fixedSize(horizontal: false, vertical: true)
        }
        .font(.callout)
    }

    private func signalText(_ s: DongleStatus) -> String {
        guard s.quality != nil else { return L("signal.not-measured") }
        guard let rsrp = s.rsrp else { return L("signal.unknown") }
        return signalWordText(signalWord(rsrp: rsrp))
    }

    private var rateText: String {
        switch model.rate {
        case .rates(let rx, let tx): return "↓ \(formatRate(rx))  ↑ \(formatRate(tx))"
        case .noInterface, .anomaly: return "↓ -  ↑ -"
        }
    }

    private func lastEndText(_ s: DongleStatus) -> String {
        var text = endResultText(s.endResult)
        if let reason = s.endReason { text += " (\(reason))" }
        if let at = s.endAt { text += " · " + L("time.ago", duration(model.now - at)) }
        return text
    }

    private var controls: some View {
        let can = ControlAvailability(model.status)
        return HStack(spacing: 8) {
            Button(L("button.pause")) { model.send(.pause) }.disabled(model.busy || !can.pause)
            Button(L("button.resume")) { model.send(.resume) }.disabled(model.busy || !can.resume)
            Button(L("button.reconnect")) { model.send(.reconnect) }.disabled(model.busy || !can.reconnect)
            if model.busy { ProgressView().controlSize(.small) }
        }
    }

    @ViewBuilder private var loginSection: some View {
        Toggle(L("login.toggle"), isOn: Binding(
            get: { model.loginStatus == .enabled || model.loginStatus == .requiresApproval },
            set: { model.setLaunchAtLogin($0) }))
            .toggleStyle(.checkbox)
        if model.loginStatus == .requiresApproval {
            Text(verbatim: L("login.requires-approval")).font(.caption).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Button(L("login.open-settings")) { model.openLoginItems() }.buttonStyle(.link).font(.caption)
        }
        if let error = model.loginError {
            Text(verbatim: error).font(.caption).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// SF Symbol for the menu bar and the state line.
func menuBarSymbol(_ status: StatusRead, now: Int64) -> String {
    switch status {
    case .missing: return "questionmark.circle"
    case .unreadable, .invalid: return "exclamationmark.triangle"
    case .ok(let s):
        if isStale(s, now: now) { return "exclamationmark.triangle" }
        switch s.state {
        case .connected: return "antenna.radiowaves.left.and.right"
        case .connecting: return "antenna.radiowaves.left.and.right.circle"
        case .waiting, .retryWait, .stopping: return "antenna.radiowaves.left.and.right.slash"
        case .paused: return "pause.circle"
        case .replugRequired, .blocked: return "exclamationmark.triangle"
        }
    }
}
