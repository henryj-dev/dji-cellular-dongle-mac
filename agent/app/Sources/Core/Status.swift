// Status file model and parser. A line-by-line port of dd_parse() in
// src/dji_dongle.c: same fixed keys, same value checks, same rejections.

public enum DongleState: String, CaseIterable, Sendable {
    case waiting, connecting, connected
    case retryWait = "retry-wait"
    case replugRequired = "replug-required"
    case paused, stopping, blocked
}

public enum SignalQuality: String, CaseIterable, Sendable {
    case ok, partial, unproven
}

public enum EndResult: String, CaseIterable, Sendable {
    case none, stopped, ended
    case cleanupRequired = "cleanup-required"
    case blocked
}

/// Parsed status; nil means null in the file.
public struct DongleStatus: Equatable, Sendable {
    public var state: DongleState = .waiting
    public var devices: Int = 0 // -1: unknown
    public var paused = false
    public var sessionStarted: Int64?
    public var connectedSince: Int64?
    public var retryAt: Int64?
    public var quality: SignalQuality? // nil: never measured
    public var rssi: Int?
    public var rsrp: Int?
    public var rsrqTenths: Int?
    public var measuredAt: Int64?
    public var endResult: EndResult = .none
    public var endReason: String?
    public var endAt: Int64?
    public var updated: Int64?
    public init() {}
}

public let statusMaxBytes = 1024
public let reasonMaxLength = 40

private let allowedBytes: Set<UInt8> = Set("{}\":,.-_abcdefghijklmnopqrstuvwxyz0123456789".utf8)

/// Byte view with C-string semantics: reading past the end yields 0.
private struct Cursor {
    let bytes: [UInt8]
    subscript(_ i: Int) -> UInt8 { i >= 0 && i < bytes.count ? bytes[i] : 0 }
    func starts(_ text: String, at i: Int) -> Bool {
        var k = i
        for c in text.utf8 { if self[k] != c { return false }; k += 1 }
        return true
    }
    /// Index just after the first `"key":`, like strstr().
    func key(_ name: String) -> Int? {
        let pattern = Array("\"\(name)\":".utf8)
        guard bytes.count >= pattern.count else { return nil }
        for start in 0...(bytes.count - pattern.count) where bytes[start] == pattern[0] {
            if Array(bytes[start..<start + pattern.count]) == pattern { return start + pattern.count }
        }
        return nil
    }
    func isEnd(_ i: Int) -> Bool { self[i] == UInt8(ascii: ",") || self[i] == UInt8(ascii: "}") }
    func isDigit(_ i: Int) -> Bool { self[i] >= UInt8(ascii: "0") && self[i] <= UInt8(ascii: "9") }
}

private enum IntKind { case value(Int64), null }

/// null, or -?[0-9]{1,12}; returns the kind and the index after it.
private func parseInt(_ c: Cursor, _ start: Int?) -> (IntKind, Int)? {
    guard var p = start else { return nil }
    if c.starts("null", at: p) { return (.null, p + 4) }
    let negative = c[p] == UInt8(ascii: "-")
    if negative { p += 1 }
    var digits = 0
    var v: Int64 = 0
    while c.isDigit(p) && digits <= 12 { v = v * 10 + Int64(c[p] - UInt8(ascii: "0")); p += 1; digits += 1 }
    if digits == 0 || digits > 12 { return nil }
    return (.value(negative ? -v : v), p)
}

/// Unix time or null; never negative.
private func parseTime(_ c: Cursor, _ key: String) -> Int64?? {
    guard let (kind, end) = parseInt(c, c.key(key)), c.isEnd(end) else { return nil }
    switch kind {
    case .null: return .some(nil)
    case .value(let v): return v >= 0 ? .some(v) : nil
    }
}

/// dBm in -999...999 or null.
private func parseSmall(_ c: Cursor, _ key: String) -> Int?? {
    guard let (kind, end) = parseInt(c, c.key(key)), c.isEnd(end) else { return nil }
    switch kind {
    case .null: return .some(nil)
    case .value(let v): return v >= -999 && v <= 999 ? .some(Int(v)) : nil
    }
}

/// "label" from the list, or null when allowed.
private func parseLabel<T: RawRepresentable & CaseIterable>(_ c: Cursor, _ key: String, nullOK: Bool) -> T??
where T.RawValue == String {
    guard let p = c.key(key) else { return nil }
    if nullOK && c.starts("null", at: p) && c.isEnd(p + 4) { return .some(nil) }
    for item in T.allCases {
        let n = item.rawValue.utf8.count
        if c[p] == UInt8(ascii: "\"") && c.starts(item.rawValue, at: p + 1) && c[p + n + 1] == UInt8(ascii: "\"") &&
            c.isEnd(p + n + 2) { return .some(item) }
    }
    return nil
}

/// The whole file must pass; any doubt rejects it (nil).
public func parseStatus(_ raw: [UInt8]) -> DongleStatus? {
    if raw.count < 2 || raw.count >= statusMaxBytes || raw.contains(0) { return nil }
    var bytes = raw
    if bytes.last == UInt8(ascii: "\n") { bytes.removeLast() }
    let c = Cursor(bytes: bytes)
    if !c.starts("{\"version\":1,", at: 0) || bytes.last != UInt8(ascii: "}") { return nil }
    if bytes.contains(where: { !allowedBytes.contains($0) }) { return nil }
    var s = DongleStatus()

    guard let stateLabel: DongleState? = parseLabel(c, "state", nullOK: false), let state = stateLabel else { return nil }
    s.state = state
    guard let (devicesKind, devicesEnd) = parseInt(c, c.key("devices")), case .value(let devices) = devicesKind,
          c.isEnd(devicesEnd), devices >= -1, devices <= 99 else { return nil }
    s.devices = Int(devices)
    guard let p = c.key("paused") else { return nil }
    if c.starts("true,", at: p) { s.paused = true } else if !c.starts("false,", at: p) { return nil }

    guard let started = parseTime(c, "session_started"), let since = parseTime(c, "connected_since"),
          let retry = parseTime(c, "retry_at"), let measured = parseTime(c, "measured_at"),
          let endAt = parseTime(c, "at"), let updated = parseTime(c, "updated"),
          let quality: SignalQuality? = parseLabel(c, "quality", nullOK: true),
          let rssi = parseSmall(c, "rssi_dbm"), let rsrp = parseSmall(c, "rsrp_dbm"),
          let resultLabel: EndResult? = parseLabel(c, "result", nullOK: false), let result = resultLabel
    else { return nil }
    s.sessionStarted = started; s.connectedSince = since; s.retryAt = retry; s.measuredAt = measured
    s.endAt = endAt; s.updated = updated; s.quality = quality; s.rssi = rssi; s.rsrp = rsrp; s.endResult = result

    // rsrq_db: null or -?[0-9]{1,2}.[0-9]
    guard var q = c.key("rsrq_db") else { return nil }
    if c.starts("null", at: q) { q += 4 } else {
        let negative = c[q] == UInt8(ascii: "-")
        if negative { q += 1 }
        var digits = 0, v = 0
        while c.isDigit(q) && digits <= 2 { v = v * 10 + Int(c[q] - UInt8(ascii: "0")); q += 1; digits += 1 }
        if digits == 0 || digits > 2 || c[q] != UInt8(ascii: ".") || !c.isDigit(q + 1) { return nil }
        v = v * 10 + Int(c[q + 1] - UInt8(ascii: "0")); q += 2
        s.rsrqTenths = negative ? -v : v
    }
    if !c.isEnd(q) { return nil }

    // reason: null or "[a-z0-9-]{1,40}"
    guard var r = c.key("reason") else { return nil }
    if c.starts("null", at: r) { r += 4 } else {
        if c[r] != UInt8(ascii: "\"") { return nil }
        r += 1
        var n = 0
        while n <= reasonMaxLength {
            let b = c[r + n]
            guard (b >= UInt8(ascii: "a") && b <= UInt8(ascii: "z")) || (b >= UInt8(ascii: "0") && b <= UInt8(ascii: "9")) ||
                  b == UInt8(ascii: "-") else { break }
            n += 1
        }
        if n == 0 || n > reasonMaxLength || c[r + n] != UInt8(ascii: "\"") { return nil }
        s.endReason = String(decoding: bytes[r..<r + n], as: UTF8.self)
        r += n + 1
    }
    return c.isEnd(r) ? s : nil
}
