// OS boundary, same rules as the native backend of src/dji_dongle.c.
// Reads link-level counters only; never reads or shows addresses.
import Darwin

public let statusPath = "/var/run/dji-cellular-dongle-mac/status.json"
public let controlPath = "/var/run/dji-cellular-dongle-mac/control.sock"
public let logPath = "/var/log/dji-cellular-dongle-mac.log"
public let replyWaitSeconds = 5

public enum StatusRead: Equatable, Sendable {
    case ok(DongleStatus)
    case missing   // ENOENT: daemon not installed or not running
    case unreadable
    case invalid
}

/// Bytes of the status file, or errno (EINVAL: not the daemon's own file).
public func readStatusBytes(path: String = statusPath, owner: uid_t = 0) -> Result<[UInt8], ErrnoError> {
    let fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
    if fd < 0 { return .failure(ErrnoError(code: errno)) }
    defer { close(fd) }
    var st = stat()
    // Only a root-owned, small regular file.
    if fstat(fd, &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG || st.st_uid != owner || st.st_size <= 0 ||
        st.st_size >= off_t(statusMaxBytes) {
        return .failure(ErrnoError(code: EINVAL))
    }
    var buffer = [UInt8](repeating: 0, count: statusMaxBytes)
    var done = 0
    while done < Int(st.st_size) {
        let count = buffer.withUnsafeMutableBytes { read(fd, $0.baseAddress! + done, statusMaxBytes - done) }
        if count < 0 && errno == EINTR { continue }
        if count <= 0 { break }
        done += count
    }
    if done == 0 { return .failure(ErrnoError(code: EINVAL)) }
    return .success(Array(buffer[0..<done]))
}

public func readStatus(path: String = statusPath, owner: uid_t = 0) -> StatusRead {
    switch readStatusBytes(path: path, owner: owner) {
    case .failure(let e): return e.code == ENOENT ? .missing : .unreadable
    case .success(let bytes): return parseStatus(bytes).map { .ok($0) } ?? .invalid
    }
}

public struct ErrnoError: Error, Equatable, Sendable { public var code: Int32 }

/// Sends one command line and returns the one reply line (blocking; call off
/// the main thread). Timeouts come back as ETIMEDOUT.
public func sendControl(_ command: ControlCommand, path: String = controlPath,
                        waitSeconds: Int = replyWaitSeconds) -> Result<String, ErrnoError> {
    var address = sockaddr_un()
    address.sun_family = sa_family_t(AF_UNIX)
    let pathBytes = Array(path.utf8)
    guard pathBytes.count < MemoryLayout.size(ofValue: address.sun_path) else { return .failure(ErrnoError(code: ENAMETOOLONG)) }
    withUnsafeMutableBytes(of: &address.sun_path) { raw in
        raw.copyBytes(from: pathBytes)
        raw[pathBytes.count] = 0
    }
    let fd = socket(AF_UNIX, SOCK_STREAM, 0)
    if fd < 0 { return .failure(ErrnoError(code: errno)) }
    defer { close(fd) }
    var one: Int32 = 1
    var wait = timeval(tv_sec: waitSeconds, tv_usec: 0)
    _ = setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
    _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, socklen_t(MemoryLayout<timeval>.size))
    _ = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wait, socklen_t(MemoryLayout<timeval>.size))
    let connected = withUnsafePointer(to: &address) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
    }
    if connected != 0 { return .failure(ErrnoError(code: errno)) }
    let line = Array((command.rawValue + "\n").utf8)
    let written = line.withUnsafeBytes { write(fd, $0.baseAddress, $0.count) }
    if written != line.count { return .failure(ErrnoError(code: written < 0 ? errno : EIO)) }
    let size = 128
    var reply = [UInt8](repeating: 0, count: size)
    var used = 0
    while used + 1 < size {
        let count = reply.withUnsafeMutableBytes { read(fd, $0.baseAddress! + used, size - 1 - used) }
        if count < 0 && errno == EINTR { continue }
        if count < 0 { return .failure(ErrnoError(code: errno == EAGAIN || errno == EWOULDBLOCK ? ETIMEDOUT : errno)) }
        if count == 0 { break }
        used += count
        if reply[0..<used].contains(UInt8(ascii: "\n")) { break }
    }
    guard let newline = reply[0..<used].firstIndex(of: UInt8(ascii: "\n")) else { return .failure(ErrnoError(code: ETIMEDOUT)) }
    return .success(String(decoding: reply[0..<newline], as: UTF8.self))
}

/// Byte counters of ppp* point-to-point interfaces; nil on error.
public func readLinkCounters() -> [LinkCounter]? {
    var all: UnsafeMutablePointer<ifaddrs>?
    if getifaddrs(&all) != 0 { return nil }
    defer { freeifaddrs(all) }
    var list: [LinkCounter] = []
    var item = all
    while let i = item, list.count < 8 {
        item = i.pointee.ifa_next
        // Link-level entries only: counters, never addresses.
        guard let addr = i.pointee.ifa_addr, Int32(addr.pointee.sa_family) == AF_LINK, let data = i.pointee.ifa_data,
              (Int32(bitPattern: i.pointee.ifa_flags) & IFF_POINTOPOINT) != 0 else { continue }
        let name = String(cString: i.pointee.ifa_name)
        guard name.hasPrefix("ppp"), name.utf8.count < Int(IFNAMSIZ) else { continue }
        let counters = data.assumingMemoryBound(to: if_data.self).pointee
        list.append(LinkCounter(name: name, rx: counters.ifi_ibytes, tx: counters.ifi_obytes))
    }
    return list
}
