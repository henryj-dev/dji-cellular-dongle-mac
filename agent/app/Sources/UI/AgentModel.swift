// Polls the status file and link counters, and runs control commands off
// the main thread.
import AppKit
import ServiceManagement

@MainActor
final class AgentModel: ObservableObject {
    @Published private(set) var status: StatusRead = .missing
    @Published private(set) var now: Int64 = 0
    @Published private(set) var rate: RateSample = .noInterface
    @Published private(set) var busy = false
    @Published private(set) var message: (text: String, isError: Bool)?
    @Published private(set) var loginStatus: SMAppService.Status = .notRegistered
    @Published private(set) var loginError: String?

    private var lastCounters: [LinkCounter]?
    private var lastSampleNanos: UInt64 = 0
    private var messageToken = 0

    init() {
        refresh()
        refreshLoginStatus()
        Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                guard let self else { return }
                self.refresh()
            }
        }
    }

    func refresh() {
        status = readStatus()
        now = Int64(time(nil))
        let nanos = clock_gettime_nsec_np(CLOCK_MONOTONIC)
        let counters = readLinkCounters()
        if let previous = lastCounters, let counters, nanos > lastSampleNanos {
            rate = linkRates(previous, counters, seconds: Double(nanos - lastSampleNanos) / 1e9)
        } else {
            rate = .noInterface
        }
        lastCounters = counters
        lastSampleNanos = nanos
    }

    /// Asks the system only when the panel opens or the toggle changes.
    func refreshLoginStatus() { loginStatus = SMAppService.mainApp.status }

    func send(_ command: ControlCommand) {
        guard !busy else { return }
        busy = true
        Task {
            // Blocking socket I/O (5 s timeouts) stays off the main actor.
            let result = await Task.detached { sendControl(command) }.value
            let outcome: ControlOutcome
            switch result {
            case .success(let reply): outcome = controlOutcome(command, reply: reply)
            case .failure(let e): outcome = controlOutcome(errno: e.code)
            }
            busy = false
            show(outcomeText(outcome), isError: outcome.isError)
            refresh()
        }
    }

    private func show(_ text: String, isError: Bool) {
        messageToken += 1
        let token = messageToken
        message = (text, isError)
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 8_000_000_000)
            guard let self, self.messageToken == token else { return }
            self.message = nil
        }
    }

    func setLaunchAtLogin(_ on: Bool) {
        loginError = nil
        do {
            if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
        } catch {
            loginError = L("login.error")
        }
        refreshLoginStatus()
    }

    func openLoginItems() { SMAppService.openSystemSettingsLoginItems() }

    func openLog() {
        let log = URL(fileURLWithPath: logPath)
        guard FileManager.default.fileExists(atPath: logPath) else { show(L("log.missing"), isError: true); return }
        // The daemon's log is root:wheel 0640: say how to read it instead of a Console error.
        guard access(logPath, R_OK) == 0 else { show(L("log.root-only"), isError: true); return }
        if let console = NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.Console") {
            NSWorkspace.shared.open([log], withApplicationAt: console, configuration: NSWorkspace.OpenConfiguration())
        } else {
            NSWorkspace.shared.open(log)
        }
    }
}
