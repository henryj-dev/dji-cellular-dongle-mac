// Localized text for the menu; every key lives in en.lproj and ko.lproj.
import Foundation

func L(_ key: String) -> String { NSLocalizedString(key, comment: "") }
func L(_ key: String, _ arguments: CVarArg...) -> String { String(format: L(key), arguments: arguments) }

let localizedDurations = DurationFormats(seconds: L("duration.seconds"), minutes: L("duration.minutes"),
                                         hours: L("duration.hours"))
func duration(_ seconds: Int64) -> String { formatDuration(seconds, localizedDurations) }

/// The state with what it means right now.
func stateLine(_ s: DongleStatus, now: Int64) -> String {
    switch s.state {
    case .connected:
        if let since = s.connectedSince { return L("state.connected.for", duration(now - since)) }
        return L("state.connected")
    case .connecting:
        if let started = s.sessionStarted { return L("state.connecting.for", duration(now - started)) }
        return L("state.connecting")
    case .retryWait:
        if let at = s.retryAt { return L("state.retry.in", duration(at - now)) }
        return L("state.retry")
    case .waiting:
        return s.devices == 0 ? L("state.waiting.none") : s.devices > 1 ? L("state.waiting.many") : L("state.waiting.unchecked")
    case .replugRequired: return L("state.replug")
    case .paused: return L("state.paused")
    case .stopping: return L("state.stopping")
    case .blocked: return L("state.blocked")
    }
}

func signalWordText(_ word: SignalWord) -> String {
    switch word {
    case .veryGood: return L("signal.very-good")
    case .normal: return L("signal.normal")
    case .weak: return L("signal.weak")
    }
}

/// "RSRP -78 dBm · RSRQ -9.5 dB · RSSI -51 dBm"; unknown parts say so.
func signalValues(_ s: DongleStatus) -> String {
    let unknown = L("signal.unknown")
    return "RSRP \(s.rsrp.map(formatDBm) ?? unknown) · RSRQ \(s.rsrqTenths.map(formatRSRQ) ?? unknown) · " +
        "RSSI \(s.rssi.map(formatDBm) ?? unknown)"
}

func endResultText(_ result: EndResult) -> String {
    switch result {
    case .none: return ""
    case .stopped: return L("end.stopped")
    case .ended: return L("end.ended")
    case .cleanupRequired: return L("end.cleanup-required")
    case .blocked: return L("end.blocked")
    }
}

func outcomeText(_ outcome: ControlOutcome) -> String {
    switch outcome {
    case .paused: return L("reply.paused")
    case .pausing: return L("reply.pausing")
    case .alreadyPaused: return L("reply.already-paused")
    case .pausedNotSaved: return L("reply.paused-not-saved")
    case .resumed: return L("reply.resumed")
    case .alreadyRunning: return L("reply.already-running")
    case .resumedNotSaved: return L("reply.resumed-not-saved")
    case .reconnecting: return L("reply.reconnecting")
    case .needAdmin: return L("reply.need-admin")
    case .refusedPaused: return L("reply.refused-paused")
    case .refusedReplug: return L("reply.refused-replug")
    case .refusedNoDevice: return L("reply.refused-no-device")
    case .refusedStopping: return L("reply.refused-stopping")
    case .notRunning: return L("problem.not-running")
    case .noReply: return L("reply.no-reply")
    case .socketError: return L("reply.socket-error")
    case .unexpectedReply: return L("reply.unexpected")
    }
}
