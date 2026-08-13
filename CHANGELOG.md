# Changelog

All notable changes to this project are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **major** — breaking change to the push API or the secrets format
- **minor** — new capability, backwards compatible
- **patch** — fixes and internal changes only

## [Unreleased]

## [1.1.0] - 2026-08-13

Display and network diagnostics. The device now drives SPI panels as well as
I2C ones, and neither a missing display nor an unreachable network can take it
down.

### Added
- **SPI display support** for 7-pin modules (`GND VCC D0/SCK D1/SDA RES DC CS`),
  selected with `DISPLAY_SPI` in `secrets.h`. Uses software SPI, so any free
  GPIOs work and no peripheral setup is needed. I2C modules are unaffected.
- **Configurable display pins** in `secrets.h` — `SDA_PIN`/`SCL_PIN` for I2C and
  `OLED_MOSI`/`OLED_CLK`/`OLED_DC`/`OLED_RST`/`OLED_CS` for SPI, with the
  ESP32-C3 safe-pin list documented alongside.
- **I2C bus scanner**, run automatically when no display is found. Sweeps all
  126 addresses in both pin orders and reports a verdict that separates
  "nothing on the bus" from "SDA and SCL reversed" from "present at another
  address".
- **WiFi diagnostics**: visible networks with RSSI, channel and security mode;
  the target AP's BSSID and auth type; and disconnect reason codes translated
  into plain language such as `wrong password`, `AP client limit`,
  `AP not found` and `AP kicked us`.
- **Station config dump** — SSID, password *length* (never the value), PMF
  capability, minimum auth mode, and the device MAC.
- **`WIFI_MAC_OVERRIDE`** diagnostic, for testing whether a router filters by
  MAC address.
- `CORE_DEBUG_LEVEL` documented in `platformio.ini` for exposing the ESP-IDF
  WiFi state machine over USB serial.

### Changed
- **The display no longer depends on the network.** Boot tries WiFi twice then
  carries on, retrying every 30 seconds in the background without ever stalling
  the render loop. Previously a router that refused the device left it stuck on
  "Connecting..." forever and it never reached the display code at all.
- WiFi retries no longer power-cycle the radio between attempts. `disconnect(true)`
  switches WiFi off entirely, which cold-started the stack on every attempt and
  made the AP see a client flapping on and off.
- The driver's auto-reconnect is disabled so there is one retry policy rather
  than two racing each other; this cut retry storms from roughly 25 events per
  30 seconds to 2.
- Connect timeout raised from 15 to 20 seconds to allow for slower DHCP, and
  modem sleep disabled.

### Fixed
- The WiFi scan diagnostic reported "SSID not visible" when the scan had in fact
  **failed**: `WIFI_SCAN_FAILED` (-2) was being treated as a network count. A
  scan cannot run while an association attempt is in flight, so the attempt is
  now stopped first, and a failed scan reports "scan failed" rather than
  asserting anything about visibility. The old behaviour produced actively
  misleading diagnoses.
- `releasing-firmware` skill: stages all outstanding work rather than two files,
  requires a bash heredoc for commit messages, documents that amending a commit
  orphans its tag, and requires `--repo` on `gh release create` — on a fork,
  `gh` otherwise targets the upstream parent repository.

## [1.0.0] - 2026-08-12

First version of the push-based architecture. Forked from
[rafbanaan/clauled](https://github.com/rafbanaan/clauled) and reworked so the
device holds no Claude credentials.

### Added
- `POST /push` ingest endpoint, authenticated with a shared `X-Clauled-Key`
  header compared in constant time.
- `GET /health` for reachability checks — exposes uptime, version, and last-push
  age, and nothing sensitive.
- mDNS advertisement, so the device is reachable at `clauled.local`.
- Compile-time configuration in a gitignored `src/secrets.h`, with a tracked
  `src/secrets.h.example` template.
- `static_assert` guards that turn missing configuration into a build error
  with an actionable message.
- Event pages, with word wrapping and a TTL, ready for notification pushes.
- Staleness tracking in the footer, shown as information rather than an error.
- Graceful degradation when the OLED is absent: the device logs the failure,
  reports `display_ok: false` from `/health`, and carries on serving pushes.
  Previously a missing display halted boot in an infinite loop, taking WiFi and
  the endpoint down with it.
- `API.md` documenting the push contract.

### Removed
- The Claude OAuth token and everything that used it — the device no longer
  contacts `api.anthropic.com` or `console.anthropic.com` at all.
- The OAuth refresh flow.
- The first-boot WiFi setup portal, which ran an **open** access point serving
  a plaintext HTTP form that you typed your token into.
- The unauthenticated browser config page and all of its write endpoints,
  along with `data/index.html` and its screenshots.
- LittleFS and the `uploadfs` flashing step — no filesystem image is needed.
- NTP and all wall-clock handling. Reset countdowns now arrive as seconds
  remaining and tick locally.

### Fixed
- README referenced a `Wire.setClock(400000)` call that did not exist in the
  source.
