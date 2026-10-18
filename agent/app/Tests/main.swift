// Offline checks for the menu bar app's pure logic. No SwiftUI/AppKit, no
// daemon, no network: a temporary tree under build/ with a file and an
// in-process Unix socket listener.
import Foundation

var checks = 0
var failures = 0
@MainActor func check(_ condition: Bool, _ label: String, line: Int = #line) {
    checks += 1
    if !condition { failures += 1; print("FAIL line \(line): \(label)") }
}

let valid = "{\"version\":1,\"state\":\"connected\",\"devices\":1,\"paused\":false,\"session_started\":1760000000," +
    "\"connected_since\":1760000010,\"retry_at\":null,\"signal\":{\"quality\":\"ok\",\"rssi_dbm\":-51,\"rsrp_dbm\":-78," +
    "\"rsrq_db\":-9.5,\"measured_at\":1760000001},\"last_end\":{\"result\":\"ended\",\"reason\":\"lcp-timeout\"," +
    "\"at\":1759999000},\"updated\":1760000011}\n"
func parse(_ text: String) -> DongleStatus? { parseStatus(Array(text.utf8)) }
func variant(_ from: String, _ to: String) -> DongleStatus? {
    precondition(valid.contains(from), "fixture lacks \(from)")
    return parse(valid.replacingFirst(from, with: to))
}
extension String {
    func replacingFirst(_ target: String, with replacement: String) -> String {
        guard let range = range(of: target) else { return self }
        return replacingCharacters(in: range, with: replacement)
    }
}

// ---- Valid status ----
if let s = parse(valid) {
    check(s.state == .connected && s.devices == 1 && !s.paused, "state, devices, paused")
    check(s.sessionStarted == 1_760_000_000 && s.connectedSince == 1_760_000_010 && s.retryAt == nil, "times")
    check(s.quality == .ok && s.rssi == -51 && s.rsrp == -78 && s.rsrqTenths == -95 && s.measuredAt == 1_760_000_001, "signal")
    check(s.endResult == .ended && s.endReason == "lcp-timeout" && s.endAt == 1_759_999_000 && s.updated == 1_760_000_011, "last end")
} else { check(false, "valid status parses") }
check(parse(String(valid.dropLast())) != nil, "no trailing newline is fine")
check(variant("\"paused\":false", "\"paused\":true")?.paused == true, "paused true")
check(variant("\"devices\":1", "\"devices\":-1")?.devices == -1, "devices -1 (unknown)")
check(variant("\"devices\":1", "\"devices\":99") != nil, "devices 99")
for state in DongleState.allCases {
    check(variant("\"connected\"", "\"\(state.rawValue)\"")?.state == state, "state \(state.rawValue)")
}
for result in EndResult.allCases {
    check(variant("\"ended\"", "\"\(result.rawValue)\"")?.endResult == result, "result \(result.rawValue)")
}
let nulls = parse(valid.replacingFirst("\"quality\":\"ok\"", with: "\"quality\":null")
    .replacingFirst("\"rssi_dbm\":-51", with: "\"rssi_dbm\":null").replacingFirst("\"rsrp_dbm\":-78", with: "\"rsrp_dbm\":null")
    .replacingFirst("\"rsrq_db\":-9.5", with: "\"rsrq_db\":null").replacingFirst("\"reason\":\"lcp-timeout\"", with: "\"reason\":null")
    .replacingFirst("\"updated\":1760000011", with: "\"updated\":null"))
check(nulls != nil && nulls?.quality == nil && nulls?.rssi == nil && nulls?.rsrp == nil && nulls?.rsrqTenths == nil &&
      nulls?.endReason == nil && nulls?.updated == nil, "nulls accepted")
check(variant("-9.5", "12.0")?.rsrqTenths == 120, "positive rsrq")
check(variant("-9.5", "-19.5")?.rsrqTenths == -195, "two-digit rsrq")
check(variant("\"rsrp_dbm\":-78", "\"rsrp_dbm\":-999")?.rsrp == -999, "rsrp -999 edge")
check(variant("lcp-timeout", String(repeating: "a", count: 40))?.endReason?.count == 40, "40-char reason")
check(variant("1760000010", "000000000000") != nil, "12 digits")
// Unknown extra keys are tolerated like the CLI (fixed keys are looked up).
check(variant("\"updated\":", "\"extra\":1,\"updated\":") != nil, "extra key tolerated as in the CLI")

// ---- Malformed variants rejected ----
let rejects: [(String, String, String)] = [
    ("\"connected\"", "\"online\"", "unknown state"),
    ("\"connected\"", "null", "null state"),
    ("\"connected\"", "\"Connected\"", "upper-case state"),
    ("\"devices\":1", "\"devices\":100", "devices > 99"),
    ("\"devices\":1", "\"devices\":-2", "devices < -1"),
    ("\"devices\":1", "\"devices\":null", "devices null"),
    ("\"devices\":1", "\"devices\":\"1\"", "devices string"),
    ("\"devices\":1", "\"devices\":1.5", "devices fraction"),
    ("\"paused\":false", "\"paused\":0", "paused number"),
    ("\"paused\":false", "\"paused\":null", "paused null"),
    ("\"session_started\":1760000000", "\"session_started\":-5", "negative time"),
    ("\"session_started\":1760000000", "\"session_started\":1234567890123", "13-digit time"),
    ("\"session_started\":1760000000", "\"session_started\":\"1760000000\"", "time as string"),
    ("\"retry_at\":null", "\"retry_at\":nul", "broken null"),
    ("\"retry_at\":null", "\"retry_at\":true", "time bool"),
    ("\"quality\":\"ok\"", "\"quality\":\"great\"", "unknown quality"),
    ("\"rssi_dbm\":-51", "\"rssi_dbm\":-1000", "rssi out of range"),
    ("\"rsrp_dbm\":-78", "\"rsrp_dbm\":1000", "rsrp out of range"),
    ("\"rsrp_dbm\":-78", "\"rsrp_dbm\":\"-78\"", "rsrp string"),
    ("-9.5", "-9", "rsrq without tenth"),
    ("-9.5", "-9.55", "rsrq two decimals"),
    ("-9.5", "-109.5", "rsrq three digits"),
    ("\"result\":\"ended\"", "\"result\":null", "null result"),
    ("\"result\":\"ended\"", "\"result\":\"crashed\"", "unknown result"),
    ("lcp-timeout", "", "empty reason"),
    ("lcp-timeout", String(repeating: "a", count: 41), "41-char reason"),
    ("lcp-timeout", "lcp_timeout", "reason with underscore"),
    ("\"reason\":\"lcp-timeout\"", "\"reason\":7", "reason number"),
    ("\"at\":1759999000", "\"at\":-1", "negative end time"),
    ("{\"version\":1,", "{\"version\":2,", "other version"),
    ("{\"version\":1,", "{ \"version\":1,", "space after brace"),
    ("lcp-timeout", "LCP", "upper case byte"),
    ("\"updated\":1760000011}", "\"updated\":1760000011}x", "text after the object"),
]
for (from, to, label) in rejects { check(variant(from, to) == nil, "rejects \(label)") }
for key in ["\"state\":", "\"devices\":", "\"paused\":", "\"session_started\":", "\"connected_since\":", "\"retry_at\":",
            "\"quality\":", "\"rssi_dbm\":", "\"rsrp_dbm\":", "\"rsrq_db\":", "\"measured_at\":", "\"result\":",
            "\"reason\":", "\"at\":", "\"updated\":"] {
    check(variant(key, "\"x\(key.dropFirst())") == nil, "rejects missing \(key)")
}
check(parse("") == nil && parse("{}") == nil, "empty and bare object")
check(parse(String(repeating: " ", count: 1100)) == nil, "too long")
check(parseStatus(Array(valid.utf8) + [0]) == nil, "NUL byte")
check(parse(valid + "\n") == nil, "two newlines")
check(parse(valid.replacingFirst("\"connected\"", with: "\"connecté\"")) == nil, "non-ASCII")

// ---- Formatting ----
check(formatDuration(0) == "0s" && formatDuration(-5) == "0s" && formatDuration(59) == "59s", "seconds")
check(formatDuration(60) == "1m 00s" && formatDuration(723) == "12m 03s" && formatDuration(3599) == "59m 59s", "minutes")
check(formatDuration(3600) == "1h 00m" && formatDuration(7500) == "2h 05m" && formatDuration(90061) == "25h 01m", "hours")
let korean = DurationFormats(seconds: "%lld초", minutes: "%lld분 %02lld초", hours: "%lld시간 %02lld분")
check(formatDuration(723, korean) == "12분 03초" && formatDuration(7500, korean) == "2시간 05분", "localized formats")
check(formatRate(0) == "0 bit/s" && formatRate(124) == "992 bit/s", "bit/s")
check(formatRate(125) == "1.00 kbit/s" && formatRate(15_000) == "120.00 kbit/s", "kbit/s")
check(formatRate(125_000) == "1.00 Mbit/s" && formatRate(1_537_500) == "12.30 Mbit/s", "Mbit/s")
check(formatRate(UInt64.max).hasSuffix("Mbit/s"), "overflow stays a number")
check(formatRSRQ(-95) == "-9.5 dB" && formatRSRQ(120) == "12.0 dB" && formatRSRQ(-5) == "-0.5 dB" && formatRSRQ(0) == "0.0 dB", "rsrq")
check(formatDBm(-78) == "-78 dBm", "dBm")

// ---- Quality words ----
check(signalWord(rsrp: -44) == .veryGood && signalWord(rsrp: -80) == .veryGood, "very good")
check(signalWord(rsrp: -81) == .normal && signalWord(rsrp: -100) == .normal, "normal")
check(signalWord(rsrp: -101) == .weak && signalWord(rsrp: -140) == .weak, "weak")

// ---- Rates, wrap and anomaly ----
let a = [LinkCounter(name: "ppp0", rx: 1000, tx: 500)]
check(linkRates(a, [LinkCounter(name: "ppp0", rx: 3000, tx: 900)], seconds: 2) == .rates(rx: 1000, tx: 200), "plain rate")
check(linkRates([LinkCounter(name: "ppp0", rx: 0xFFFF_FF00, tx: 0xFFFF_FFF0)],
                [LinkCounter(name: "ppp0", rx: 0x100, tx: 0x10)], seconds: 1) == .rates(rx: 0x200, tx: 0x20), "wrap once")
check(linkRates(a, [LinkCounter(name: "ppp0", rx: 1000 + 0x8000_0000, tx: 500)], seconds: 1) == .anomaly, "rx jump 2^31")
check(linkRates(a, [LinkCounter(name: "ppp0", rx: 1000, tx: 499)], seconds: 1) == .anomaly, "tx going back")
check(linkRates(a, [LinkCounter(name: "ppp0", rx: 1000 + 0x7FFF_FFFF, tx: 500)], seconds: 1) ==
      .rates(rx: 0x7FFF_FFFF, tx: 0), "just below 2^31")
check(linkRates(a, [LinkCounter(name: "ppp1", rx: 5000, tx: 5000)], seconds: 1) == .noInterface, "other interface")
check(linkRates([], [], seconds: 1) == .noInterface, "no interfaces")
check(linkRates(a, a, seconds: 0) == .noInterface, "zero interval")
check(linkRates(a + [LinkCounter(name: "ppp1", rx: 0, tx: 0)],
                [LinkCounter(name: "ppp0", rx: 2000, tx: 500), LinkCounter(name: "ppp1", rx: 10, tx: 20)], seconds: 1) ==
      .rates(rx: 1010, tx: 20), "sum of interfaces")

// ---- Control replies and buttons ----
check(controlOutcome(.pause, reply: "ok") == .paused && controlOutcome(.pause, reply: "ok ending-session") == .pausing, "pause ok")
check(controlOutcome(.pause, reply: "ok already-paused") == .alreadyPaused, "already paused")
check(controlOutcome(.resume, reply: "ok already-running") == .alreadyRunning, "already running")
check(controlOutcome(.pause, reply: "ok not-saved") == .pausedNotSaved &&
      controlOutcome(.resume, reply: "ok not-saved") == .resumedNotSaved, "not saved")
check(controlOutcome(.reconnect, reply: "ok") == .reconnecting && controlOutcome(.resume, reply: "ok") == .resumed, "ok")
check(controlOutcome(.reconnect, reply: "ok not-saved") == .unexpectedReply, "not-saved for reconnect")
check(controlOutcome(.pause, reply: "error=paused") == .unexpectedReply, "paused error for pause")
check(controlOutcome(.reconnect, reply: "error=paused") == .refusedPaused &&
      controlOutcome(.reconnect, reply: "error=replug-required") == .refusedReplug &&
      controlOutcome(.reconnect, reply: "error=no-device") == .refusedNoDevice, "reconnect refusals")
check(controlOutcome(.resume, reply: "error=stopping") == .refusedStopping, "stopping")
check(controlOutcome(.pause, reply: "error=denied") == .needAdmin, "denied")
check(controlOutcome(.pause, reply: "OK") == .unexpectedReply && controlOutcome(.pause, reply: "ok ") == .unexpectedReply, "strict")
check(controlOutcome(errno: EACCES) == .needAdmin && controlOutcome(errno: EPERM) == .needAdmin, "EACCES")
check(controlOutcome(errno: ENOENT) == .notRunning && controlOutcome(errno: ECONNREFUSED) == .notRunning, "ENOENT")
check(controlOutcome(errno: ETIMEDOUT) == .noReply && controlOutcome(errno: EIO) == .socketError, "timeout and other")
check(!ControlOutcome.paused.isError && ControlOutcome.needAdmin.isError && ControlOutcome.unexpectedReply.isError, "isError")
var st = parse(valid)!
func can(_ s: DongleStatus) -> ControlAvailability { ControlAvailability(.ok(s)) }
check(can(st).pause && !can(st).resume && can(st).reconnect, "connected buttons")
st.paused = true; st.state = .paused
check(!can(st).pause && can(st).resume && !can(st).reconnect, "paused buttons")
st.paused = false; st.state = .replugRequired
check(can(st).pause && !can(st).reconnect, "replug buttons")
st.state = .blocked
check(can(st).pause && !can(st).resume && !can(st).reconnect, "blocked: no reconnect")
st.state = .waiting; st.devices = 0
check(!can(st).reconnect, "no dongle")
st.state = .stopping
check(!can(st).pause && !can(st).resume && !can(st).reconnect, "stopping")
let invalidCan = ControlAvailability(.invalid)
check(invalidCan.pause && invalidCan.resume && invalidCan.reconnect, "invalid file keeps buttons (daemon decides)")
check(!ControlAvailability(.missing).pause && !ControlAvailability(.unreadable).reconnect, "no file or unreadable")

// ---- Stale status (heartbeat 60 s, stale after 180 s, future slack 60 s) ----
var fresh = parse(valid)! // updated 1760000011
check(!isStale(fresh, now: 1_760_000_011 + 180) && isStale(fresh, now: 1_760_000_011 + 181), "stale after 180 s")
check(!isStale(fresh, now: 1_760_000_011 - 60) && isStale(fresh, now: 1_760_000_011 - 61), "future by more than 60 s")
fresh.updated = nil
check(!isStale(fresh, now: 1_900_000_000), "null updated is never stale")
let staleButtons = can(parse(valid)!)
check(staleButtons.pause && staleButtons.reconnect, "stale keeps buttons (availability ignores time)")

// ---- Helpers for the in-process listener ----
func listenUnix(_ path: String) -> Int32 {
    var address = sockaddr_un()
    address.sun_family = sa_family_t(AF_UNIX)
    let bytes = Array(path.utf8)
    guard bytes.count < MemoryLayout.size(ofValue: address.sun_path) else { return -1 }
    withUnsafeMutableBytes(of: &address.sun_path) { $0.copyBytes(from: bytes); $0[bytes.count] = 0 }
    let fd = socket(AF_UNIX, SOCK_STREAM, 0)
    guard fd >= 0 else { return -1 }
    let bound = withUnsafePointer(to: &address) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
    }
    if bound != 0 || listen(fd, 4) != 0 { close(fd); return -1 }
    return fd
}
func readLine(_ fd: Int32) -> String {
    var bytes: [UInt8] = []
    var byte: UInt8 = 0
    while bytes.count < 64, read(fd, &byte, 1) == 1 { bytes.append(byte); if byte == UInt8(ascii: "\n") { break } }
    return String(decoding: bytes, as: UTF8.self)
}
/// Accepts one client on a background thread and runs the handler on it.
func serveOnce(_ listener: Int32, _ handler: @escaping @Sendable (Int32) -> Void) -> DispatchSemaphore {
    let done = DispatchSemaphore(value: 0)
    Thread.detachNewThread {
        let client = accept(listener, nil, nil)
        if client >= 0 { handler(client); close(client) }
        done.signal()
    }
    return done
}

// ---- Native file read (owner = this user), sun_path limits and a socket round trip ----
// Run from the repository root, like the C tests: the tree lives under build/.
var template = Array("build/agent_app_test.XXXXXX".utf8CString)
let made = template.withUnsafeMutableBufferPointer { buffer in mkdtemp(buffer.baseAddress!).map { String(cString: $0) } }
check(made != nil, "temporary directory")
if let dir = made {
    let file = dir + "/status.json", link = dir + "/link.json"
    let me = getuid()
    func put(_ text: String) {
        let fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
        _ = Array(text.utf8).withUnsafeBytes { write(fd, $0.baseAddress, $0.count) }
        close(fd)
    }
    check(readStatus(path: file, owner: me) == .missing, "missing file")
    put(valid)
    if case .ok(let s) = readStatus(path: file, owner: me) { check(s.state == .connected, "reads file") }
    else { check(false, "reads file") }
    check(readStatus(path: file, owner: me &+ 1) == .unreadable, "wrong owner refused")
    put("{\"version\":1}")
    check(readStatus(path: file, owner: me) == .invalid, "invalid file")
    put("")
    check(readStatus(path: file, owner: me) == .unreadable, "empty file refused")
    put(valid)
    _ = symlink(file, link)
    check(readStatus(path: link, owner: me) == .unreadable, "symlink not followed")
    check(readStatus(path: dir, owner: me) == .unreadable, "directory refused")
    let sock = dir + "/control.sock"
    if case .failure(let e) = sendControl(.pause, path: sock) { check(controlOutcome(errno: e.code) == .notRunning, "no socket") }
    else { check(false, "no socket") }

    // sun_path holds 103 bytes plus NUL: longer is refused before connecting.
    let longest = "build/" + String(repeating: "x", count: 103 - 6)
    if case .failure(let e) = sendControl(.pause, path: longest) { check(e.code == ENOENT, "103-byte path is tried") }
    else { check(false, "103-byte path is tried") }
    if case .failure(let e) = sendControl(.pause, path: longest + "x") { check(e.code == ENAMETOOLONG, "104-byte path refused") }
    else { check(false, "104-byte path refused") }

    // In-process listener on the temporary tree.
    let listener = listenUnix(sock)
    check(listener >= 0, "listener")
    if listener >= 0 {
        // (a) The reply comes in two writes; the command line is checked.
        var done = serveOnce(listener) { client in
            let line = readLine(client)
            let first = line == "pause\n" ? "ok " : "error=bad\n"
            _ = Array(first.utf8).withUnsafeBytes { write(client, $0.baseAddress, $0.count) }
            usleep(100_000)
            _ = Array("already-paused\n".utf8).withUnsafeBytes { write(client, $0.baseAddress, $0.count) }
        }
        let split = sendControl(.pause, path: sock, waitSeconds: 5)
        done.wait()
        if case .success(let reply) = split { check(controlOutcome(.pause, reply: reply) == .alreadyPaused, "reply in two writes") }
        else { check(false, "reply in two writes") }
        // (b) Accepted but never answered: the 1 s wait ends as no reply.
        done = serveOnce(listener) { client in
            var byte: UInt8 = 0
            while read(client, &byte, 1) > 0 {} // until the client gives up and closes
        }
        let start = clock_gettime_nsec_np(CLOCK_MONOTONIC)
        let silent = sendControl(.reconnect, path: sock, waitSeconds: 1)
        let waited = Double(clock_gettime_nsec_np(CLOCK_MONOTONIC) - start) / 1e9
        done.wait()
        if case .failure(let e) = silent { check(controlOutcome(errno: e.code) == .noReply, "silent daemon is no reply") }
        else { check(false, "silent daemon is no reply") }
        check(waited >= 0.9 && waited < 4, "timeout honoured (\(waited) s)")
        close(listener)
    }
    unlink(sock); unlink(link); unlink(file)
    check(rmdir(dir) == 0, "temporary tree removed")
}

print("agent-app checks=\(checks) failures=\(failures)")
exit(failures == 0 ? 0 : 1)
