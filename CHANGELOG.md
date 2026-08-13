# Changelog

All notable changes to this project are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **major** — breaking change to the push API or the secrets format
- **minor** — new capability, backwards compatible
- **patch** — fixes and internal changes only

## [Unreleased]

## [3.4.0] - 2026-08-13

The header is just the session now, centred. Model and effort move to the
footer. Cost is gone.

```
      clauled-pusher
────────────────────────────────
5h reset      2h44m        15%
█████░░░░░░░░░░░░░░░░░░░░░░░░░░░
ctx         357k/1M        45%
██████████████░░░░░░░░░░░░░░░░░░
/ Running Bash
────────────────────────────────
Sonnet 5                 xhigh
```

### Changed
- **Header is `session` alone, centred.** It no longer shares the row with the
  model - the header's one job now is telling you which work this is.
- **Footer is `title` (the model) on the left, effort on the right.** Both
  field names are unchanged from v3.3.0; only where they render moved. Cost
  used to occupy the footer's left slot; it duplicated a number Claude Code's
  own UI already shows, so that slot now carries the model instead.

### Removed
- **`footer.left` (cost).** The device no longer reads or draws it. A pusher
  that still sends it does no harm - the value is simply ignored, same as any
  other field this firmware does not recognise.

### Notes
- No new fields, no renamed fields. A pusher older than this release still
  renders correctly: `session` and `title` land in the same places they always
  did on the wire, only the SCREEN POSITION of `title` changed.

## [3.3.0] - 2026-08-13

The header returns, and the four identity fields each get their own corner.

```
clauled-pusher          Opus 5
────────────────────────────────
5h reset      4h25m         7%
███░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
ctx         357k/1M        45%
██████████████░░░░░░░░░░░░░░░░░░
/ Running Bash
────────────────────────────────
$0.11                    xhigh
```

### Added
- **A header row**: the session on the left, the model on the right.
- **`session`** — a new top-level string, rendered at the header's left. Where
  the model says *what* is answering, this says *which work* it is answering
  about, which matters as soon as more than one session is open.
- **`footer.right`** — the effort level, spelled out. It has the corner to
  itself now, so `xhigh` no longer has to be squeezed to `xhi`.

### Changed
- **`title` is the model alone.** It previously carried model and effort joined,
  which meant a long model name truncated the effort away entirely. They are
  separate fields in separate corners, so neither can crowd out the other.
- **Bars return to 6 pixels**, as they were before v3.1.0. This is the cost of
  the header coming back: v3.1.0 bought 11-pixel bars by removing it, and that
  trade is now reversed. Knowing which session the numbers belong to was judged
  worth more than bar legibility.

### Notes
- **The schema is unchanged — `v` is still 3.** Both new fields are optional
  additions, so a pusher older than v3.2.0 still renders correctly: its combined
  `"Opus 5 xhigh"` simply lands in the header's right, and the footer's right
  stays empty. Nothing breaks; the effort is just less prominent.
- `session_name` is the natural source for the header's left, but Claude Code
  sends it rarely — once in fifty payloads, in the captures behind this. The
  plugin falls back to the workspace directory name, which is nearly always
  present and arguably the more useful answer.

## [3.2.0] - 2026-08-13

### Changed
- **The two data rows are now three columns**: label flush left, detail
  centred, percentage flush right.

  ```
  5h reset      4h25m         7%
  ███░░░░░░░░░░░░░░░░░░░░░░░░░░░
  ctx         357k/1M        45%
  ██████████████░░░░░░░░░░░░░░░░
  ```

  Previously all three ran together as one left-aligned string. Separating them
  gives each a fixed place to look, which is the point of a glanceable display.

  The detail is centred on the **screen**, not on the gap between its
  neighbours, so it stays put as the percentage widens from `7%` to `100%` — a
  value that shifts every time the number beside it changes is harder to read at
  a glance than one that never moves.

  It moves only to avoid a collision, and then it nudges rather than overlaps:
  there is always at least one blank character on each side. With `5h reset` at
  48 px, true centring would leave a 1-pixel gap, so the quota row's detail sits
  5 px right of centre. The context row centres exactly. A guaranteed gap is
  worth more than pixel-perfect symmetry — 1 px reads as two words touching.

  If even the nudge will not fit, the detail is dropped and the percentage
  survives. That is the right way round: the percentage is what you came for.

### Notes
- No pusher change is required. `gauge.label`, the paired `row` side and
  `gauge.pct` are the same three fields as before; only their placement changed.
- The 21-character line budget still applies per column rather than to a
  composed string, so labels have more room than they did in v3.1.0.

## [3.1.0] - 2026-08-13

A denser screen. Dropping the top header and the USB indicator paid for bars
nearly twice as tall, and sleep no longer takes the display away from you.

### Changed
- **New layout.** Every one of the 64 rows is now budgeted:

  ```
   0- 7   text    5h reset 4h33m 55%
   9-19   bar     quota                    (11 px, was 6)
  21-28   text    ctx 357k/1M 45%
  30-40   bar     context                  (11 px, was 6)
  43-53   status  banner / spinner / sleep
  55      rule
  56-63   text    Opus 5 xhigh      $0.11
  ```

  Each gauge is one text line and one bar, and the model, effort and cost move
  to the bottom, where they never change shape and work as an anchor for the
  eye. A 6-pixel bar with a 4-pixel fill was hard to read from across a desk;
  this is the change that fixes that.
- **Gauge lines are composed on the device**, from the gauge label, its paired
  `row` detail and its percentage. The 21-character budget is now enforced
  where the pixels are rather than trusted to the host. If the line will not
  fit, the countdown is dropped before the percentage — 100% is one character
  wider than every other value, and it is exactly when the row most needs to
  be readable.
- **The schema is unchanged.** `v` is still 3 and every field means what it
  meant; only their positions moved. No pusher change is required, though
  v3.1.0 of the plugin sends shorter labels that suit the new rows.

### Removed
- **The `USB ok` / `USB idle` indicator.** It could never distinguish "Claude
  Code is closed" from "the cable is unplugged", and the status row already
  says whether anything is happening.
- **The top header row.** The model and effort it carried now live at the
  bottom next to the cost.

### Fixed
- **Sleep no longer takes over the whole screen.** It is confined to the status
  row, so the gauges stay readable while the host is away — which is the one
  time you are most likely to glance over casually, and previously the one time
  there was nothing to see. The cost is the animation: an 11-pixel row cannot
  hold the growing `Z`s of the old full-screen version, since a size-2 glyph is
  already 16 pixels tall. What remains is a horizontal march with a slight
  rise, alongside the sleeping face and how long it has been quiet.

### Documentation
- **The README claimed v3.0.0 as the latest release** through the whole of
  v3.0.1, and `API.md`'s status example reported the same stale number. The
  release process is supposed to move `version.h`, `CHANGELOG.md` and
  `README.md` together; it did not.
- **The `flashing-firmware` skill still described the pre-v2.0.0 architecture.**
  It asked for WiFi credentials and a push key, told you to create
  `src/secrets.h`, and verified the flash by curling `clauled.local/health` —
  none of which has existed since v2.0.0. Anyone following it would have gone
  looking for a file deleted two releases ago. It now covers the real process,
  and requires checking the reported `version=` against `version.h`: a device
  can otherwise run a fix while reporting an older number, which is exactly
  what happened between v3.0.0 and v3.0.1 and left the version string
  untrustworthy for diagnosis.

### Notes
- Both data rows are tight. `5h reset 4h33m 100%` is 19 characters and
  `ctx 357k/1M 100%` is 16, against a 21-character line. The plugin's selftest
  asserts both at 100% so a label change cannot quietly overflow them.

## [3.0.1] - 2026-08-13

### Fixed
- **A banner masked the spinner for its full lifetime.** Events cleared `busy`,
  but `busy` did not clear events, so a `Your turn` banner held the detail row
  for its entire 5-minute TTL while every spinner push landed invisibly beneath
  it. The display looked frozen mid-task even though hooks were firing normally.
  Going busy now clears any banner: if Claude is working, it is not your turn.

## [3.0.0] - 2026-08-13

Everything on one screen: two gauges, a live activity line, and a sleep
animation. The protocol moves to labelled display fields, so the host decides
what each gauge means and new data sources need no firmware change.

### Changed — breaking
- **Protocol carries labelled fields, not fixed metrics.** `gauge1`, `gauge2`,
  `row`, `footer`, `title` and `busy` replace the `usage.five_hour` /
  `seven_day` buckets. Schema version is now `3`.
- **Page cycling is gone.** Everything renders on a single screen, so
  `CYCLE_TIME`, `SHOW_WEEKLY_SONNET` and `SHOW_UPTIME` have been removed from
  `config.h`.

### Added
- **Two gauges.** The host labels them; the plugin uses them for the 5h
  subscription quota and context-window occupancy.
- **Activity line.** The detail row shows what Claude is doing —
  `/ Running Bash`, `- Editing main.cpp`, or a gerund while it thinks. The
  spinner animates on device time, so a long turn keeps moving with no pushes.
- **Sleep animation.** After `STALE_AFTER_S` with no push the device decides the
  host is asleep or Claude Code is closed and switches to a `(-_-)` face with
  Z's drifting up and away. It cannot tell which, and does not pretend to.
- **Attention banners** are inverted — white block, black text — deliberately
  the loudest thing on the screen.
- The BOOT button clears a stuck banner or spinner immediately.

### Fixed
- The busy state self-expires after 3 minutes, so a missed `Stop` hook cannot
  leave the spinner running forever.

### Notes
- Detail row priority is banner → spinner → static detail, so an alert always
  wins over activity.
- A gauge with `pct` below zero renders `--` with an empty bar. One feed being
  unavailable never blanks the other.

## [2.0.0] - 2026-08-13

The device moves to USB serial and drops networking entirely. It now holds no
credentials of any kind — not a Claude token, not a WiFi password, not a shared
key. The payload schema is unchanged; only the transport differs.

### Changed — breaking
- **Transport is USB serial, not HTTP.** The device reads newline-delimited
  JSON from the serial port and answers each line with a single-line JSON
  acknowledgement. `POST /push` and `GET /health` are gone.
  `{"v":1,"cmd":"status"}` replaces the health endpoint.
- **`secrets.h` is gone.** Configuration moved to `src/config.h`, which is
  tracked in git because nothing in it is secret. There is no copy step and
  nothing to fill in before flashing.
- The payload schema is unchanged — the same `v`, `usage`, `resets_in` and
  `events` fields, and the same merge semantics — so a pusher only has to
  change how it sends, not what.

### Removed
- The entire WiFi stack: station mode, reconnection handling, mDNS, the web
  server, and every diagnostic built for them (I2C bus scanning is retained;
  WiFi scanning, disconnect-reason decoding, MAC override and PMF/auth-mode
  reporting are not).
- WiFi credentials and the shared push key. The device holds no credentials, so
  release binaries no longer contain anything sensitive.
- Endpoint authentication, which physical USB access replaces.

### Added
- `{"v":1,"cmd":"status"}` probe returning firmware version, display state,
  uptime and last-push age, so a host can confirm a port really is a Clauled
  device before pushing to it.
- Human-readable log output is prefixed with `# ` so it is distinguishable from
  protocol replies, which always begin with `{`.
- The footer shows USB link state (`USB ok` / `USB idle`) in place of the IP.

### Impact
- Flash usage dropped from 64.4% to **22.7%** (844,594 -> 297,606 bytes)
- RAM usage dropped from 12.7% to **4.4%** (41,628 -> 14,412 bytes)

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
