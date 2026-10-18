# dji-cellular-dongle-mac

Use a **DJI Cellular Dongle 2** (USB `2ca3:4009`, reports itself as a Fibocom NL668 modem) as an LTE modem on macOS, without any vendor driver.

Plug the dongle in and the Mac goes online over LTE; unplug it and the Mac returns to its previous network.

> **Status: experimental.** Tested on one Mac (Apple Silicon, macOS 26.6) with one dongle and one SIM. It runs as root and changes the Mac's default route and DNS while the dongle is plugged in. Read [Risks](#risks) before installing.

Unofficial project; not affiliated with or endorsed by DJI. DJI is a trademark of SZ DJI Technology Co., Ltd.

[한국어 안내](README.ko.md)

## How it works

- `ppp_lte_up` (root) opens the dongle's AT/PPP interface from user space (IOKit, no kernel extension), dials the data call and runs the macOS system `pppd` (`/usr/sbin/pppd`) over it.
- `pppd` installs the default route and the carrier's DNS for the whole Mac (`defaultroute usepeerdns`) and publishes them under one fixed SystemConfiguration service ID. LCP echo detects a dead link within about 40 seconds.
- When the session ends for any reason (Ctrl+C, unplug, link loss), the leftover SystemConfiguration keys of that fixed service ID are removed, and macOS falls back to Wi-Fi or Ethernet.
- `ppp_lte_upd` is a small launchd daemon. It waits for the dongle, runs each session as a fresh `ppp_lte_up --up` child and reconnects with backoff (5 s doubling to 5 min) while the dongle stays plugged in.

## Requirements

- macOS with `/usr/sbin/pppd` (present on current macOS releases) and the Xcode Command Line Tools (`xcode-select --install`).
- A DJI Cellular Dongle 2 with a SIM that has a data plan. The APN must already be stored in the modem (the dongle's own setup normally does this); this tool does not change the APN, PIN or USB mode.
- No PPP configuration files: `/etc/ppp/options`, `/etc/ppp/ip-up`, other files in `/etc/ppp/`, or `/var/root/.ppprc` make the session refuse to start. No other `pppd` (for example an L2TP VPN) may be running.

## Build and test

```sh
make                 # builds build/ppp_lte_up and build/ppp_lte_upd
make verify-offline  # all unit/integration tests with fakes; no device, root or network
```

## Try it once (no install)

```sh
sudo build/ppp_lte_up --up
```

Wait for `up=connected ...`, use the network, then press Ctrl+C. The last line `up-result=stopped ...` and exit code 0 mean a clean stop. If a run ends with `next-run-cleanup=required`, run `sudo build/ppp_lte_up --cleanup`.

## Install (plug in = online)

```sh
make
sudo make install
```

This installs:

| Path | What |
|---|---|
| `/Library/PrivilegedHelperTools/dji-cellular-dongle-mac/ppp_lte_up`, `ppp_lte_upd` | binaries (root:wheel) |
| `/Library/LaunchDaemons/io.github.henryj-dev.dji-cellular-dongle-mac.plist` | launchd job (starts at boot) |
| `/etc/newsyslog.d/dji-cellular-dongle-mac.conf` | log rotation |
| `/var/log/dji-cellular-dongle-mac.log` | log (fixed labels and counts only) |

macOS shows a "background item added" notice. Turning the item off in System Settings > General > Login Items stops the daemon.

Remove everything (the log file is kept):

```sh
sudo make uninstall
```

## Behaviour

| Event | What happens |
|---|---|
| Dongle plugged in | Connects in about 6–10 s; all traffic of the Mac goes over LTE |
| Dongle unplugged | Session stops, leftover keys removed, previous network comes back |
| Link drops while plugged in | Clean up, then reconnect after 5 s, 10 s, … up to 5 min |
| Attempts fail right away three times (modem stuck in data mode after a hard stop) | Logs `upd=replug-required` and waits; unplug and plug the dongle again (keep it out for more than a second) |
| Two dongles | Waits; only one dongle is supported |
| `launchctl bootout` / shutdown | Stops the session cleanly (up to about 60 s), then cleans up |

The daemon checks that its child binary and every directory above it are root-owned and not writable by others before each run.

## Status and control (optional)

The daemon keeps a small status file and a control socket. The `dji-dongle` command-line tool reads and drives them:

```sh
make agent                 # builds build/dji-dongle and build/DJI Dongle.app
sudo make install-agent    # installs /usr/local/bin/dji-dongle and /Applications/DJI Dongle.app
dji-dongle status          # state, connection time, signal at connect
dji-dongle watch           # the same plus live RX/TX rate every second (Ctrl+C to quit)
dji-dongle pause           # end the session and stay off until resume (admin account)
dji-dongle resume
dji-dongle reconnect       # end the session and start a fresh one at once
```

- `status` and `watch` read `/var/run/dji-cellular-dongle-mac/status.json` (readable by everyone; fixed labels and numbers only, no IP addresses or identifiers). `dji-dongle status --json` prints it as is. The daemon rewrites it on every state change, and at least once a minute otherwise, and removes it when it stops cleanly. If the file has not been rewritten for more than 3 minutes (or its time is more than a minute ahead), `status` and `watch` add `— status may be stale (daemon not updating)` to the state: the daemon may have crashed or hung.
- `pause`, `resume` and `reconnect` go through `/var/run/dji-cellular-dongle-mac/control.sock` (root:admin, 0660, inside the root-only status directory). The daemon also checks the caller: root or a member of the admin group. Others get `need an admin account`.
- A pause is saved in `/Library/Application Support/dji-cellular-dongle-mac/paused` and survives restarts. It ends a running session the same way a stop does (up to 60 s). If that file or its directory cannot be verified at start, the daemon starts paused (no unexpected data use) and logs `upd=pause-file-blocked paused=yes`. `resume` does not clear a required replug: unplug and plug the dongle in again.
- Each accepted command is logged as `upd=control command=... uid-class=root|admin`.
- `sudo make uninstall-agent` removes the tool and the app. `sudo make uninstall` also removes the socket, the status file and a saved pause.

### Menu bar app

`DJI Dongle.app` (macOS 13 or later) puts the same information in the menu bar. It needs the command-line tools' `swiftc` to build (no Xcode), is signed ad hoc, and has no Dock icon. It is built for the Mac it is built on (Apple silicon or Intel), not as a universal app. Open it once from /Applications.

- The icon shows the state: antenna (connected), crossed-out antenna (waiting, retrying or stopping), antenna in a circle (connecting), pause (paused), warning (replug required, blocked, an unreadable status file, or a status that may be stale), question mark (daemon not installed or not running).
- The panel shows the state with the connection time or the retry countdown, the **signal at connect** (RSRP, RSRQ, RSSI and a word: very good at RSRP -80 dBm or better, normal down to -100 dBm, weak below), the live ↓/↑ rate of the `ppp*` link, and how the last session ended. A status not rewritten for more than 3 minutes adds "Status may be stale (daemon not updating)"; the buttons stay on and the daemon decides.
- The signal is measured once, when a session starts, not continuously. A value from hours ago still describes the start of the current session.
- **Pause**, **Resume** and **Reconnect** need an administrator account, like the command-line tool; other accounts see "This needs an administrator account". Buttons that the current state cannot use are greyed out (Reconnect is off while paused, blocked or waiting for a replug). If the status file cannot be parsed, all three stay on.
- **Launch at login** uses the system's login items. If macOS asks for approval, allow the app in System Settings › General › Login Items. **Open Log** opens `/var/log/dji-cellular-dongle-mac.log` in Console when your account can read it; the installed log is root-only (0640), so the app usually shows the `sudo tail -f` command instead.
- The app reads the status file every 2 s and the byte counters of `ppp*` interfaces. It never reads or shows IP addresses, opens no network connections, runs no other programs and needs no privileges.
- Quit the app before reinstalling or uninstalling it (`sudo make install-agent` / `sudo make uninstall-agent`), and turn off **Launch at login** before uninstalling.

## Tested so far

On the test Mac (Apple Silicon, macOS 26.6) with the daemon installed:

- Plug in: connected in about 7 s; default route on `ppp0`, HTTPS works.
- Unplug: session stops, leftover keys removed, Wi-Fi becomes primary again.
- Plug in again: reconnects automatically.
- With the `adb` server running: stays connected (`children-check tolerated creator=adb`).
- `sudo make uninstall`: removes the daemon and files.
- Measured with `networkQuality`: **download about 3.6 Mbps, upload about 11 Mbps, idle latency about 70 ms** (first release: 1.6 / 8 Mbps, 270 ms). The relay now keeps four USB reads queued and loops about 700 times a second; data still arrives more slowly than it is read, so the remaining limit is most likely the LTE link at the test location or the modem's PPP (dial-up) mode. A phone with the same SIM is faster at the same place and the plan is not throttled, so the modem's PPP mode is the likely limit. Each session logs one `up-signal ...` line (CSQ RSSI, LTE RSRP/RSRQ) before dialing. See `docs/ppp-design.md`.
- Not tested: sleep/wake, long sessions (hours), other macOS versions or Macs.

## Risks

- **All traffic goes over LTE while the dongle is plugged in**, including background updates and cloud sync. Watch your data plan.
- While connected, IPv6 is expected to be blocked (pppd publishes an IPv6 placeholder primary). During the modem's IPCP renegotiation a short window may route over Wi-Fi.
- It runs as root and uses the system `pppd`. If the process is killed hard (power loss), SystemConfiguration keys can be left behind until the next start, which cleans them up automatically, or until `sudo /Library/PrivilegedHelperTools/dji-cellular-dongle-mac/ppp_lte_up --cleanup`.
- Binaries are ad-hoc signed by your own build. No notarized package is published yet.
- The remote end of the PPP link and the carrier PDP context are not explicitly torn down; the modem handles that.

## Limitations

- One dongle model, one interface layout. Other modems are refused.
- No network service order: while plugged in, LTE always wins over Wi-Fi. (A DriverKit serial driver that exposes the dongle as a normal modem port would allow ordering; not implemented.)
- No sharing to other devices (NAT), no SMS, no APN/PIN configuration.
- Sleep/wake behaviour is untested.
- The session refuses to share the dongle's interface. On the test Mac the Android Debug Bridge server (`adb`, e.g. from Android Studio) kept creating a second user client on it; that one client is now tolerated while the session holds its exclusive open (`adb` cannot do any I/O then). Any other process doing the same still stops the session with `children-check reason=extra-child ... creator=<name>` in the log.

## Other tools in this repository

`build/dji-lte-mac` (descriptor diagnostics and fixed read-only AT probes), `build/ppp_lte_once` (one fixed echo test) and `build/ppp_auth_trial` (local pppd test without the dongle) were used during development. See [docs/history/](docs/history/) (Korean) and [docs/ppp-design.md](docs/ppp-design.md).

## License

MIT, see [LICENSE](LICENSE).
