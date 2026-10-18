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

## Tested so far

On the test Mac (Apple Silicon, macOS 26.6) with the daemon installed:

- Plug in: connected in about 7 s; default route on `ppp0`, HTTPS works.
- Unplug: session stops, leftover keys removed, Wi-Fi becomes primary again.
- Plug in again: reconnects automatically.
- With the `adb` server running: stays connected (`children-check tolerated creator=adb`).
- `sudo make uninstall`: removes the daemon and files.
- Measured with `networkQuality`: **download about 1.9 Mbps, upload about 11.6 Mbps, idle latency about 120 ms** (first release: 1.6 / 8 Mbps, 270 ms). Diagnostics show the bottleneck is this tool's USB relay loop, not the modem: only one USB read is in flight at a time and each relay loop takes about 4 ms. Multiple in-flight reads and event-driven waiting are planned next (see `docs/ppp-design.md`).
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
