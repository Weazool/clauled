# Clauled

**Latest release:** v3.7.0 — see [CHANGELOG.md](CHANGELOG.md). The running firmware reports its own version via the serial status probe.

A small desk gadget built on an ESP32-C3 with an OLED screen that shows your Claude subscription usage at a glance.

## How it works

The device holds **no credentials of any kind** and has no network stack. It plugs in over USB and reads newline-delimited JSON from the serial port:

```
Claude Code (desktop)  ──JSON lines over USB──▶  Clauled  ──▶  OLED
```

No token, no WiFi, nothing to leak — the device is already tethered to the machine that feeds it, so the network was never earning its keep. Wire protocol: [API.md](API.md). Desktop side: [clauled-pusher](https://github.com/Weazool/clauled-pusher).

## What you need

- ESP32-C3 Mini (also sold as ESP32-C3 SuperMini)
- SH1106 OLED, 128x64 — must be **SH1106**, not SSD1306, they look identical but need different drivers. Both **I2C (4-pin)** and **SPI (7-pin)** modules work.
- Four or seven jumper wires, depending on the module
- USB-C cable — a **data** cable, not charge-only

## Wiring

Count the pins first: four is I2C, seven (with `CS`) is SPI.

**I2C — 4-pin** (leave `DISPLAY_SPI` commented out in `src/config.h`)

| OLED pin | ESP32-C3 pin |
|----------|--------------|
| GND      | GND          |
| VCC      | 3.3V         |
| SDA      | GPIO 4       |
| SCL      | GPIO 5       |

**SPI — 7-pin** (default)

| OLED pin | ESP32-C3 pin |
|----------|--------------|
| GND       | GND     |
| VCC       | 3.3V    |
| D1 / SDA  | GPIO 4  |
| D0 / SCK  | GPIO 5  |
| DC        | GPIO 6  |
| RES       | GPIO 7  |
| CS        | GPIO 10 |

All pins configurable in `src/config.h`. Safe GPIOs: 0, 1, 3, 4, 5, 6, 7, 10 — avoid 2/8 (boot strapping), 9 (BOOT button), 11–17 (SPI flash), 18/19 (USB).

> [!NOTE]
> I2C acknowledges, so a missing/miswired I2C display reports `display_ok: false`. SPI is write-only and always reports `true` — only pixels confirm it's working.

## Configuration

`src/config.h` is tracked in git, nothing secret in it. Flash as-is unless your wiring differs.

| Setting | Notes |
|---|---|
| `DISPLAY_SPI` | Defined for 7-pin SPI; comment out for 4-pin I2C |
| `OLED_MOSI` / `OLED_CLK` / `OLED_DC` / `OLED_RST` / `OLED_CS` | SPI pins |
| `SDA_PIN` / `SCL_PIN` / `OLED_ADDR` | I2C pins and address |
| `STALE_AFTER_S` | No push for a session this long → shows it as quiet (default 300) |
| `SESSION_GONE_S` | No push for a session this long → drops it from the roster (default 900) |
| `ROTATE_INTERVAL_S` | Multiple sessions active → cycle this often (default 3) |
| `QUIET_IDLE_S` | No push from ANY session this long **during quiet hours** → panel powers off (default 900) |

## Flashing

```bash
python -m pip install --user platformio
pio run --target upload
pio device monitor
```

If `pio` isn't on PATH after install: `%APPDATA%\Python\Python3xx\Scripts\pio.exe` on Windows. If upload fails with `ClearCommError`, just retry — the C3's native USB re-enumerates mid-flash.

## What shows on the screen

```
clauled-pusher                 2/5
────────────────────────────────
5h            4h33m        55%
███████████████████░░░░░░░░░░░░░
ctx         357k/1M        45%
███████████████░░░░░░░░░░░░░░░░░
/ Running Bash
────────────────────────────────
Sonnet 5                 xhigh
```

Two gauges — account quota, context window — each a three-column line (label, centred detail, percentage) above a bar. Header is the session on the left, `N/M` on the right. Footer is model left, effort right.

**The device tracks up to 8 Claude Code sessions at once** and decides on its own which to show — no host coordination needed beyond tagging each push with which session it came from. Priority: any session needing your attention beats any session merely working beats everything else, and it cycles every 3 seconds within whichever group is active. `N/M` is your position among *every* active session, not just the ones being cycled — two sessions needing attention out of five shows `2/5` then `4/5`, never resetting to a smaller-looking `1/2`.

**Row 1 alternates too** — the 5h quota and the weekly (7-day, all models) quota trade places in the same slot every few seconds, entirely independent of which session is on screen, since the quota belongs to your account, not to any one session.

**Middle row is the status line**: what Claude is doing (`/ Running Bash`), or when it's your turn, **the whole screen inverts** — `Your turn` / `Claude needs input`, the loudest signal the device has. BOOT clears whichever session is currently shown, not the whole roster — dismissing one does not silently dismiss a different session's pending banner.

**A session idle 5+ minutes** shows `(-_-)` and how long it's been quiet in its own status row, gauges still visible; past 15 minutes it drops out of the roster. **Once every session has aged out**, the header reads `Idle`, the quota row keeps alternating, and a bigger idle graphic fills the lower half — the account quota and the last known model are not really "session data," so they do not disappear along with the roster. **During quiet hours**, idle past `QUIET_IDLE_S` with nothing from *any* session, the panel powers fully off (`SH110X_DISPLAYOFF`) — configure the window on the [clauled-pusher](https://github.com/Weazool/clauled-pusher) side. Any push wakes it instantly; BOOT forces a brief flash.

## Sending it data

Anything that writes to a serial port. See [API.md](API.md) for the protocol, or install [clauled-pusher](https://github.com/Weazool/clauled-pusher) for the real thing.

```bash
node -e "const{openSync,writeSync,closeSync}=require('fs');const fd=openSync('\\\\\\\\.\\\\COM8','w');writeSync(fd,JSON.stringify({v:3,gauge2:{label:'Context',pct:74}})+'\n');closeSync(fd)"
```

## Troubleshooting

**Screen blank on I2C.** Send `{"v":3,"cmd":"status"}`, check `display_ok`. `false` → confirm 3.3V and that SDA/SCL (GPIO 4/5) aren't swapped.

**Garbage on screen.** Almost always SSD1306 mistaken for SH1106.

**Nothing on the port.** Cable is probably charge-only.

**`ClearCommError` on upload.** Retry. Still failing: hold **BOOT**, tap **RESET**, release **BOOT**.

**Port busy.** Close any other serial monitor.

**Asleep.** No push for `STALE_AFTER_S` — normal when Claude Code is closed.

**Completely dark, not even the sleeping face.** Check `quiet_sleep` on the status probe — `true` means quiet-hours power-down working as intended. Any push wakes it.

**Gauge 1 shows `--`.** No `rate_limits` yet — arrives on a later render.

**Header shows `Idle` instead of a session.** Every session has been quiet for 15+ minutes and dropped from the roster — normal once Claude Code closes or a session goes untouched that long. Check `sessions` on the status probe; any new push brings it back.

**Wrong session shown, or it won't stop rotating.** Check `sessions` on the status probe — if it is higher than you expect, an old session has not aged out yet (up to 15 minutes), or several genuinely have something happening at once. It always shows the highest-priority group (attention, then working, then idle) and rotates within it every 3s; a single active session never rotates.

## License

MIT.
