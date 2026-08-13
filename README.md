# Clauled

**Latest release:** v3.1.0 — see [CHANGELOG.md](CHANGELOG.md). The running firmware reports its own version via the serial status probe.

A small desk gadget built on an ESP32-C3 with an OLED screen that shows your Claude subscription usage at a glance.

## How it works

The device holds **no credentials of any kind** — no Claude token, no WiFi password — and has no network stack at all. It plugs into your computer over USB and reads newline-delimited JSON from the serial port:

```
Claude Code (desktop)  ──JSON lines over USB──▶  Clauled  ──▶  OLED
```

That design is deliberate, and it arrived by elimination. The original version put a Claude OAuth token on the device and had it poll the API directly — a long-lived credential in flash on a microcontroller, reachable over an unauthenticated LAN page. Moving to a push model removed the token. Moving to USB then removed the WiFi credentials, the network stack, the LAN endpoint, and every class of problem that came with them.

The device is already tethered to the machine that feeds it. The network was never earning its keep.

The wire protocol is in [API.md](API.md). The desktop side is [clauled-pusher](https://github.com/Weazool/clauled-pusher).

## Why

The claude.ai usage page is fine, but you have to go look at it. I wanted a thing on my desk that just shows me how much headroom I have left.

## What you need

- ESP32-C3 Mini (also sold as ESP32-C3 SuperMini)
- SH1106 OLED, 128x64. Must be the **SH1106** controller, not the SSD1306 — they look identical from the outside and need different drivers. Both **I2C (4-pin)** and **SPI (7-pin)** modules are supported.
- Four or seven jumper wires, depending on the module
- USB-C cable — a **data** cable, not charge-only

## Wiring

Count the pins on your module first. Four pins is I2C; seven (with `CS`) is SPI. Note that SPI modules commonly label the clock `SCK` and MOSI `SDA`, which is easy to mistake for I2C.

**I2C — 4-pin module** (leave `DISPLAY_SPI` commented out in `src/config.h`)

| OLED pin | ESP32-C3 pin |
|----------|--------------|
| GND      | GND          |
| VCC      | 3.3V         |
| SDA      | GPIO 4       |
| SCL      | GPIO 5       |

**SPI — 7-pin module** (default)

| OLED pin | ESP32-C3 pin |
|----------|--------------|
| GND       | GND     |
| VCC       | 3.3V    |
| D1 / SDA  | GPIO 4  |
| D0 / SCK  | GPIO 5  |
| DC        | GPIO 6  |
| RES       | GPIO 7  |
| CS        | GPIO 10 |

All pins are configurable in `src/config.h`. Safe GPIOs on the ESP32-C3 are 0, 1, 3, 4, 5, 6, 7 and 10 — avoid 2 and 8 (boot strapping), 9 (BOOT button), 11–17 (SPI flash) and 18/19 (USB, which you need for flashing).

> [!NOTE]
> I2C acknowledges, so a missing or miswired I2C display is detected and reported as `display_ok: false`. **SPI does not** — it is write-only, so `display_ok` is always `true` in SPI mode. Only pixels on the glass confirm an SPI display is working.

## Configuration

There is nothing to fill in. `src/config.h` is tracked in git and contains no secrets — only which display you have and how it is wired. Flash it as-is unless your wiring differs from the table above.

| Setting | Notes |
|---|---|
| `DISPLAY_SPI` | Defined for a 7-pin SPI module; comment out for 4-pin I2C |
| `OLED_MOSI` / `OLED_CLK` / `OLED_DC` / `OLED_RST` / `OLED_CS` | SPI pins |
| `SDA_PIN` / `SCL_PIN` / `OLED_ADDR` | I2C pins and address |
| `STALE_AFTER_S` | No push for this long → the device falls asleep (default 300) |

## Flashing

A PlatformIO project. Open it in VS Code with the PlatformIO extension, or install the CLI:

```bash
python -m pip install --user platformio
```

```bash
pio run --target upload
```

```bash
pio device monitor
```

If `pio` is not found after a `--user` install it is on disk but not on PATH — on Windows that is `%APPDATA%\Python\Python3xx\Scripts\pio.exe`.

If the upload fails to connect with `ClearCommError` or "the device does not recognize the command", just run it again — see troubleshooting.

## What shows on the screen

Everything on one screen — no page cycling.

```
5h reset 4h33m 55%
███████████████████░░░░░░░░░░░░░
ctx 357k/1M 45%
███████████████░░░░░░░░░░░░░░░░░
/ Running Bash
────────────────────────────────
Opus 5 xhigh              $0.11
```

Two gauges: your 5h subscription quota and how full the context window is. Each is one text line — label, its most useful companion number, and the percentage — above a bar. The bottom row carries model, effort and session cost, and never changes shape, so it works as an anchor for the eye.

The bars are 11 pixels tall. They used to be 6, with a 4-pixel fill, which was hard to read from across a desk; dropping the old top header and the USB indicator paid for the difference.

**The middle row is the status line.** While Claude is working it shows what it is doing — `/ Running Bash`, `- Editing main.cpp`, or a gerund like `\ Discombobulating` while it thinks. When Claude wants you, it becomes an inverted banner reading `Your turn` or `Claude needs input`, deliberately the loudest thing on the screen.

**When nothing has arrived for five minutes**, the device decides the host is asleep or Claude Code is closed, and the status row shows a `(-_-)` face, how long it has been quiet, and drifting z's. It cannot tell which of the two happened, and does not pretend to. The gauges stay on screen throughout — being away is exactly when you are most likely to glance over casually, and a full-screen sleep animation showed you nothing at all.

Both text rows are tight against the 21-character line. If a line will not fit, the device drops the countdown before the percentage — `100%` is one character wider than every other value, and that is precisely when the row most needs to be readable.

The spinner and the sleep animation run on device time, so they keep moving with no pushes at all. The BOOT button clears a stuck banner or spinner.

## Sending it data

Anything that can write to a serial port works. See [API.md](API.md) for the full protocol.

```bash
node -e "const{openSync,writeSync,closeSync}=require('fs');const fd=openSync('\\\\\\\\.\\\\COM8','w');writeSync(fd,JSON.stringify({v:3,gauge2:{label:'Context',pct:74}})+'\n');closeSync(fd)"
```

For normal use, install [clauled-pusher](https://github.com/Weazool/clauled-pusher) — a Claude Code plugin that finds the device automatically and feeds it usage and notifications.

## Troubleshooting

**Screen stays blank on an I2C module.** Send `{"v":3,"cmd":"status"}` and check `display_ok`. If it is `false` the display did not answer at `0x3C` — confirm VCC is on 3.3V and that SDA (GPIO 4) and SCL (GPIO 5) are not swapped. The device keeps working regardless. On SPI, `display_ok` tells you nothing; check wiring against the SPI table.

**Garbage or noise on screen.** Almost always an SSD1306 module rather than an SH1106.

**Nothing appears on the port.** Confirm the USB cable carries data — charge-only cables power the board but never enumerate it.

**Upload fails with `ClearCommError`.** Expected on this board: esptool resets the C3 and its native USB re-enumerates, invalidating the port handle. Run the upload again; the second attempt usually succeeds. If it keeps failing, hold **BOOT**, tap **RESET**, release **BOOT**.

**Port is busy.** Only one program can hold a serial port. Close `pio device monitor` or any other terminal before pushing.

**It went to sleep.** No push has arrived for `STALE_AFTER_S`. Normal when Claude Code is closed or the PC slept — the pusher only runs while Claude Code is open. It wakes on the next push.

**Gauge 1 shows `--`.** No 5h figure has reached the device yet. Claude Code sends one in the statusline payload's `rate_limits` block, but not on every invocation, so it can take a few renders — see the [clauled-pusher](https://github.com/Weazool/clauled-pusher) README.

## License

MIT.
