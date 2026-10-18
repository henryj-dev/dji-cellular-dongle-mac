// Pure formatting and decisions shared by the app and its test binary.
// Mirrors dd_duration, dd_rate, dd_rates and dd_control in src/dji_dongle.c.
import Foundation

/// printf templates for durations; the app passes localized ones.
public struct DurationFormats: Sendable {
    public var seconds: String      // %lld
    public var minutes: String      // %lld %02lld
    public var hours: String        // %lld %02lld
    public static let english = DurationFormats(seconds: "%llds", minutes: "%lldm %02llds", hours: "%lldh %02lldm")
    public init(seconds: String, minutes: String, hours: String) {
        self.seconds = seconds; self.minutes = minutes; self.hours = hours
    }
}

/// "45s", "12m 03s", "2h 05m"; negative counts as 0.
public func formatDuration(_ value: Int64, _ formats: DurationFormats = .english) -> String {
    let s = max(value, 0)
    if s < 60 { return String(format: formats.seconds, s) }
    if s < 3600 { return String(format: formats.minutes, s / 60, s % 60) }
    return String(format: formats.hours, s / 3600, s % 3600 / 60)
}

/// Bit rate text from bytes per second, like the CLI.
public func formatRate(_ bytesPerSecond: UInt64) -> String {
    let (bits, overflow) = bytesPerSecond.multipliedReportingOverflow(by: 8)
    let b = overflow ? UInt64.max : bits
    if b < 1000 { return "\(b) bit/s" }
    if b < 1_000_000 { return String(format: "%.2f kbit/s", Double(b) / 1e3) }
    return String(format: "%.2f Mbit/s", Double(b) / 1e6)
}

public func formatDBm(_ value: Int) -> String { "\(value) dBm" }

/// RSRQ from tenths of a dB, e.g. -105 -> "-10.5 dB".
public func formatRSRQ(_ tenths: Int) -> String {
    let t = abs(tenths)
    return "\(tenths < 0 ? "-" : "")\(t / 10).\(t % 10) dB"
}

public enum SignalWord: Equatable, Sendable { case veryGood, normal, weak }

/// RSRP >= -80 very good, -80 down to -100 normal, below -100 weak.
public func signalWord(rsrp: Int) -> SignalWord {
    if rsrp >= -80 { return .veryGood }
    if rsrp >= -100 { return .normal }
    return .weak
}

/// Byte counters of one ppp* interface (32-bit, may wrap).
public struct LinkCounter: Equatable, Sendable {
    public var name: String
    public var rx: UInt32
    public var tx: UInt32
    public init(name: String, rx: UInt32, tx: UInt32) { self.name = name; self.rx = rx; self.tx = tx }
}

public enum RateSample: Equatable, Sendable {
    case rates(rx: UInt64, tx: UInt64) // bytes per second
    case noInterface
    case anomaly // a jump of 2^31 bytes or more: interface recreated
}

/// Rates over `seconds` for interfaces present in both samples; one wrap per
/// counter is handled, a delta of 2^31 or more is an anomaly.
public func linkRates(_ first: [LinkCounter], _ second: [LinkCounter], seconds: Double) -> RateSample {
    var rx: UInt64 = 0, tx: UInt64 = 0, found = false
    for z in second {
        for a in first where a.name == z.name {
            let inBytes = z.rx &- a.rx, outBytes = z.tx &- a.tx
            if inBytes >= 0x8000_0000 || outBytes >= 0x8000_0000 { return .anomaly }
            rx += UInt64(inBytes); tx += UInt64(outBytes)
            found = true
        }
    }
    if !found || !(seconds > 0) { return .noInterface }
    return .rates(rx: UInt64((Double(rx) / seconds).rounded()), tx: UInt64((Double(tx) / seconds).rounded()))
}

public enum ControlCommand: String, CaseIterable, Sendable { case pause, resume, reconnect }

/// What a control request came to; the app turns it into localized text.
public enum ControlOutcome: Equatable, Sendable {
    case paused, pausing, alreadyPaused, pausedNotSaved
    case resumed, alreadyRunning, resumedNotSaved
    case reconnecting
    case needAdmin, refusedPaused, refusedReplug, refusedNoDevice, refusedStopping
    case notRunning, noReply, socketError, unexpectedReply

    public var isError: Bool {
        switch self {
        case .paused, .pausing, .alreadyPaused, .pausedNotSaved, .resumed, .alreadyRunning, .resumedNotSaved, .reconnecting:
            return false
        default: return true
        }
    }
}

/// Transport failure by errno, as the CLI words it.
public func controlOutcome(errno code: Int32) -> ControlOutcome {
    switch code {
    case EACCES, EPERM: return .needAdmin
    case ENOENT, ECONNREFUSED: return .notRunning
    case ETIMEDOUT: return .noReply
    default: return .socketError
    }
}

/// Daemon reply line for a command. Unknown replies are never shown.
public func controlOutcome(_ command: ControlCommand, reply: String) -> ControlOutcome {
    switch (reply, command) {
    case ("ok", .pause): return .paused
    case ("ok", .resume): return .resumed
    case ("ok", .reconnect): return .reconnecting
    case ("ok ending-session", .pause): return .pausing
    case ("ok already-paused", .pause): return .alreadyPaused
    case ("ok already-running", .resume): return .alreadyRunning
    case ("ok not-saved", .pause): return .pausedNotSaved
    case ("ok not-saved", .resume): return .resumedNotSaved
    case ("error=denied", _): return .needAdmin
    case ("error=paused", .reconnect): return .refusedPaused
    case ("error=replug-required", .reconnect): return .refusedReplug
    case ("error=no-device", .reconnect): return .refusedNoDevice
    case ("error=stopping", _): return .refusedStopping
    default: return .unexpectedReply
    }
}

/// The daemon rewrites the file at least every 60 s: older than 180 s, or more
/// than 60 s in the future, means it may not be updating. Null is never stale.
public let staleAfterSeconds: Int64 = 180
public let futureSlackSeconds: Int64 = 60
public func isStale(_ status: DongleStatus, now: Int64) -> Bool {
    guard let updated = status.updated else { return false }
    return now - updated > staleAfterSeconds || updated - now > futureSlackSeconds
}

/// Which buttons make sense right now (the daemon still decides). A file that
/// does not parse keeps them on; no file or an unreadable one turns them off.
public struct ControlAvailability: Equatable, Sendable {
    public var pause: Bool
    public var resume: Bool
    public var reconnect: Bool
    public init(_ read: StatusRead) {
        switch read {
        case .invalid:
            pause = true; resume = true; reconnect = true
        case .missing, .unreadable:
            pause = false; resume = false; reconnect = false
        case .ok(let s):
            if s.state == .stopping { pause = false; resume = false; reconnect = false; return }
            pause = !s.paused
            resume = s.paused
            reconnect = !s.paused && s.state != .replugRequired && s.state != .blocked && s.devices != 0
        }
    }
}
