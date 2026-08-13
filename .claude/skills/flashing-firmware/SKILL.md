---
name: flashing-firmware
description: Use when flashing Clauled to the ESP32-C3 - first-time setup, reflashing after code changes, or changing the display wiring in src/config.h
---

# Flashing Firmware

## Overview

Builds the firmware, uploads it to the board over USB, and verifies the device
actually came up running the version you just built.

**There is nothing to configure and no credentials to collect.** Since v2.0.0 the
device holds no WiFi password, no Claude token, and no push key — configuration
lives in `src/config.h`, which is tracked in git because none of it is secret.

If you find yourself looking for `src/secrets.h`, you are working from a
pre-v2.0.0 memory. It does not exist.

## When to Use

- "flash the device", "reflash", "upload the firmware"
- After any firmware change that needs to reach the board
- After changing display wiring or `STALE_AFTER_S` in `src/config.h`

## Preflight

**1. Find PlatformIO.** It is not always on PATH. Try in order:

```bash
pio --version || python -m platformio --version || ~/.platformio/penv/Scripts/pio.exe --version
```

Use whichever answers, and keep using that same form for every command below.
If none answer, PlatformIO must be installed first — offer either
`pip install platformio` or the VS Code PlatformIO extension, and confirm which
before installing anything.

**2. Identify the board.**

```bash
python -m platformio device list
```

The Clauled device is the one with **`VID:PID=303A:1001`** — Espressif. Match on
the vendor ID, not on the description: `USB Serial Device (COMn)` is also what a
Logitech receiver and a dozen other things call themselves, and flashing the
wrong device is worth avoiding.

If no `303A` device appears, the board is not enumerating. Check the cable
carries data — charge-only cables power the board but never enumerate it.

## Build

```bash
python -m platformio run
```

Do not upload a build that did not succeed.

Sanity-check the size line against the last release; a sudden jump means
something was pulled in that should not have been. As of v3.0.1 it is roughly
**RAM 4.4%, Flash 22.4%**.

## Upload

```bash
python -m platformio run --target upload --upload-port COM8
```

Pass `--upload-port` explicitly with the port identified above. Auto-detection
picks the first serial device, which on a machine with several is a coin flip.

**Close anything holding the port first** — `pio device monitor`, a serial
terminal, or an Arduino IDE window. Only one process can own it.

**If upload fails to connect:** this board uses native USB CDC
(`ARDUINO_USB_MODE=1`, `CDC_ON_BOOT=1`) and may need manual download mode — hold
BOOT, tap RESET, release BOOT. That is physical, so the user has to do it. Ask
them to, then retry.

## Verify — do not skip

A successful upload proves bytes moved, nothing more. Probe the device:

```bash
cd ../clauled-pusher && node bin/doctor.mjs
```

Expect:

```
device        found at \\.\COM8
status probe  replied
              version=3.0.1 display_ok=true uptime=8s last_push_age=1
push          display push written
              event push written
```

Check all three:

| Field | Means |
|---|---|
| `version=` | **must match `src/version.h`** — see below |
| `display_ok=true` | the panel was found; `false` means the device runs headless |
| `uptime=` | a small number, confirming it really did just reboot |

**The version check is the one that gets skipped, and it is the one that bites.**
A device can be running your fix while still reporting an older number, because
it was flashed before `version.h` was bumped. The build is then untraceable:
`doctor` says 3.0.0, the changelog says 3.0.1, and the next person to diagnose a
problem is working from a version string that is a lie. If they do not match,
reflash — do not rationalise the difference.

Without the pusher checked out, probe by hand:

```bash
python -m platformio device monitor --baud 115200
```

Expect a `# [boot] Clauled v...` line. Human-readable logs are prefixed `# `;
protocol replies always begin with `{`. The monitor does not exit on its own —
run it with a timeout.

Finally, **confirm the user can see it on the glass.** `doctor` leaves two gauges
and a test banner on screen. Upload succeeding and the OLED being dark are
entirely compatible states.

## Common Mistakes

| Mistake | Fix |
|---|---|
| Looking for `src/secrets.h` | Gone since v2.0.0. Config is `src/config.h`, tracked, nothing secret. |
| Reporting success on upload alone | Upload only proves bytes moved; probe for `version=` and look at the screen |
| Not checking `version=` against `version.h` | A stale version stamp makes every later diagnosis unreliable |
| Letting PlatformIO pick the port | Match `VID:PID=303A:1001` and pass `--upload-port` |
| Flashing with a monitor open | Only one process can own the port; close it first |
| Assuming a failed connect means a dead board | Native USB CDC often just needs manual download mode |
