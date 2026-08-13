# Clauled

**Latest release:** v2.0.0 — see [CHANGELOG.md](CHANGELOG.md). The running firmware reports its own version via the serial status probe.

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
| `CYCLE_TIME` | Seconds per page; `0` for manual (BOOT button) |
| `SHOW_WEEKLY_SONNET` | Separate Sonnet weekly bucket, for Max plans |
| `SHOW_UPTIME` | Adds a device-uptime page |
| `STALE_AFTER_S` | No push for this long → footer marks the data stale |

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

```
Claude                  1/2
Current session         16%
####........................
       Resets in 1h 21m
─────────────────────────────
42s                   USB ok
```

Title left, percent used right, bar underneath, reset countdown centered. The footer shows time since the last push on the left and the USB link state on the right. Pages cycle automatically, or manually with the BOOT button.

Countdowns tick locally between pushes — the device has no wall clock and needs none, because `resets_in` arrives as seconds remaining rather than a timestamp.

## Sending it data

Anything that can write to a serial port works. See [API.md](API.md) for the full protocol.

```bash
node -e "const{openSync,writeSync,closeSync}=require('fs');const fd=openSync('\\\\\\\\.\\\\COM8','w');writeSync(fd,JSON.stringify({v:1,usage:{five_hour:{pct:23.5,resets_in:4920}}})+'\n');closeSync(fd)"
```

For normal use, install [clauled-pusher](https://github.com/Weazool/clauled-pusher) — a Claude Code plugin that finds the device automatically and feeds it usage and notifications.

## Troubleshooting

**Screen stays blank on an I2C module.** Send `{"v":1,"cmd":"status"}` and check `display_ok`. If it is `false` the display did not answer at `0x3C` — confirm VCC is on 3.3V and that SDA (GPIO 4) and SCL (GPIO 5) are not swapped. The device keeps working regardless. On SPI, `display_ok` tells you nothing; check wiring against the SPI table.

**Garbage or noise on screen.** Almost always an SSD1306 module rather than an SH1106.

**Nothing appears on the port.** Confirm the USB cable carries data — charge-only cables power the board but never enumerate it.

**Upload fails with `ClearCommError`.** Expected on this board: esptool resets the C3 and its native USB re-enumerates, invalidating the port handle. Run the upload again; the second attempt usually succeeds. If it keeps failing, hold **BOOT**, tap **RESET**, release **BOOT**.

**Port is busy.** Only one program can hold a serial port. Close `pio device monitor` or any other terminal before pushing.

**Footer says "stale" or "USB idle".** No push has arrived for `STALE_AFTER_S`. Normal when Claude Code is closed — the pusher only runs while it is open.

## License

MIT.
