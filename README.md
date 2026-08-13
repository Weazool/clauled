# Clauled

**Latest release:** v1.1.0 — see [CHANGELOG.md](CHANGELOG.md). The running firmware reports its own version at `GET /health`.

A small desk gadget built on an ESP32-C3 with an OLED screen that shows your Claude subscription usage at a glance.

## How it works

The device holds **no Claude credentials** and never contacts Anthropic. It sits on your network exposing a single authenticated endpoint, and a pusher on your desktop sends it the numbers:

```
Claude Code (desktop)  ──POST /push, shared key──▶  Clauled  ──▶  OLED
```

That inversion is deliberate. An earlier design put a Claude OAuth token on the device and had it poll the API directly, which meant a long-lived credential sitting in flash on a microcontroller, reachable over an unauthenticated LAN web page. Now the token stays on your machine, where it already lives and where TLS is done properly. The device is just a screen.

The wire contract is in [API.md](API.md).

## Why

The claude.ai usage page is fine, but you have to go look at it. I wanted a thing on my desk that just shows me how much headroom I have left.

## What you need

- ESP32-C3 Mini (also sold as ESP32-C3 SuperMini)
- SH1106 OLED, 128x64. Must be the **SH1106** controller, not the SSD1306 — they look identical from the outside and need different drivers. Both **I2C (4-pin)** and **SPI (7-pin)** modules are supported.
- Four or seven jumper wires, depending on the module
- USB-C cable

## Wiring

Count the pins on your module first. Four pins is I2C; seven (with `CS`) is SPI. Note that SPI modules commonly label the clock `SCK` and MOSI `SDA`, which is easy to mistake for I2C.

**I2C — 4-pin module** (leave `DISPLAY_SPI` commented out)

| OLED pin | ESP32-C3 pin |
|----------|--------------|
| GND      | GND          |
| VCC      | 3.3V         |
| SDA      | GPIO 4       |
| SCL      | GPIO 5       |

**SPI — 7-pin module** (define `DISPLAY_SPI`)

| OLED pin | ESP32-C3 pin |
|----------|--------------|
| GND       | GND     |
| VCC       | 3.3V    |
| D1 / SDA  | GPIO 4  |
| D0 / SCK  | GPIO 5  |
| DC        | GPIO 6  |
| RES       | GPIO 7  |
| CS        | GPIO 10 |

All pins are configurable in `secrets.h`. Safe GPIOs on the ESP32-C3 are 0, 1, 3, 4, 5, 6, 7 and 10 — avoid 2 and 8 (boot strapping), 9 (BOOT button), 11–17 (SPI flash) and 18/19 (USB, which you need for flashing).

> [!NOTE]
> I2C acknowledges, so a missing or miswired I2C display is detected and reported as `display_ok: false`. **SPI does not** — it is write-only, so `display_ok` is always `true` in SPI mode. Only pixels on the glass confirm an SPI display is working.

## Configuration

All configuration is compile-time. There is no setup portal and no config web page.

```bash
cp src/secrets.h.example src/secrets.h
```

Then edit `src/secrets.h`:

| Setting | Notes |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | Required. Blank values are a **compile error**, not a boot-time surprise. |
| `CLAULED_PUSH_KEY` | Shared secret the pusher sends as `X-Clauled-Key`. Required. |
| `CLAULED_HOSTNAME` | mDNS name, default `clauled` → `http://clauled.local` |
| `DISPLAY_SPI` | Define for a 7-pin SPI module; leave commented for 4-pin I2C |
| `OLED_MOSI` / `OLED_CLK` / `OLED_DC` / `OLED_RST` / `OLED_CS` | SPI pins |
| `SDA_PIN` / `SCL_PIN` | I2C pins |
| `CYCLE_TIME` | Seconds per page; `0` for manual (BOOT button) |
| `SHOW_WEEKLY_SONNET` | Separate Sonnet weekly bucket, for Max plans |
| `SHOW_UPTIME` | Adds a device-uptime page |
| `STALE_AFTER_S` | No push for this long → footer marks the data stale |

Generate a push key with:

```bash
node -e "console.log(require('crypto').randomBytes(24).toString('base64url'))"
```

> [!WARNING]
> `src/secrets.h` is gitignored and must **never** be committed — it holds your WiFi password. `src/secrets.h.example` is the tracked template; keep real values out of it. Note that the compiled firmware binary contains your WiFi credentials, so don't share build artifacts either.

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

There is no filesystem image any more, so no `uploadfs` step. If the upload fails with a chip mismatch, check the board is set to `esp32-c3-devkitm-1`.

## What shows on the screen

```
Claude                  1/2
Current session         16%
####........................
       Resets in 1h 21m
─────────────────────────────
42s              192.168.1.34
```

Title left, percent used right, bar underneath, reset countdown centered. The footer shows time since the last push on the left and the device IP on the right. Pages cycle automatically, or manually with the BOOT button.

Countdowns tick locally between pushes — the device has no wall clock and needs none, because `resets_in` arrives as seconds remaining rather than a timestamp.

## Sending it data

```bash
curl -X POST http://clauled.local/push \
  -H "X-Clauled-Key: <your key>" \
  -H "Content-Type: application/json" \
  -d '{"v":1,"usage":{"five_hour":{"pct":23.5,"resets_in":4920}}}'
```

See [API.md](API.md) for the full contract, including event notifications.

Use `curl` rather than PowerShell's `Invoke-WebRequest` for pushes — see troubleshooting below.

## Checking it is alive

```bash
curl http://clauled.local/health
```

```json
{"ok":true,"version":"1.0.0","display_ok":true,"uptime":250,"last_push_age":0,"schema":1}
```

No key required — `/health` exposes nothing sensitive. `display_ok` is your wiring check, `last_push_age` is seconds since the last accepted push (`-1` means never), and `version` tells you what is actually flashed so a stale board is obvious.

**A missing display is not fatal.** If the OLED is not detected the device logs it, reports `display_ok: false`, and carries on serving pushes — so a loose I2C wire stays diagnosable over the network instead of requiring a USB cable.

## Troubleshooting

**Garbage or noise on screen.** Almost always an SSD1306 module rather than an SH1106. If you are certain it is an SH1106, try lowering the I2C clock by adding `Wire.setClock(100000)` after `Wire.begin()` in `src/main.cpp`.

**Screen stays blank.** Check `curl http://clauled.local/health` first. If `display_ok` is `false` the display did not answer at `0x3C` — confirm VCC is on 3.3V and that SDA (GPIO 4) and SCL (GPIO 5) are not swapped. The rest of the device keeps working regardless.

**Upload fails with `ClearCommError` or "the device does not recognize the command".** Expected on this board — esptool resets the C3 and its native USB re-enumerates, invalidating the port handle. Simply run the upload again; the second attempt usually succeeds. If it keeps failing, force download mode by hand: hold **BOOT**, tap **RESET**, release **BOOT**.

**Pushes time out from PowerShell, but the device accepted them anyway.** `Invoke-WebRequest` and `Invoke-RestMethod` send `Expect: 100-continue` on POSTs with a body. The ESP32 web server never sends the `100 Continue`, so the client waits for a reply that is not coming while the server processes the push regardless. This looks exactly like an auth failure and is not one. Use `curl.exe` for `POST /push`; GET requests such as `/health` are unaffected.

**WiFi never connects.** The ESP32-C3 has no 5 GHz radio. Confirm `WIFI_SSID` names a 2.4 GHz network — pointing it at a 5 GHz SSID fails silently apart from the serial log.

**Build fails with "Set WIFI_SSID in src/secrets.h".** Working as intended — you have not filled in `src/secrets.h` yet, or you never copied it from the example.

**Screen says "Waiting for data".** WiFi is up but nothing has pushed yet. Confirm with `curl http://clauled.local/health`, then check your pusher.

**`clauled.local` does not resolve.** Use the IP shown in the footer instead. Some networks block mDNS between wireless and wired segments.

**Pushes return 401.** The `X-Clauled-Key` header does not match `CLAULED_PUSH_KEY` in `src/secrets.h`. Remember the device needs reflashing after you change that value.

**Footer says "stale".** No push has arrived for `STALE_AFTER_S`. Normal if Claude Code is closed — the pusher only runs while it is open.

## License

MIT.
