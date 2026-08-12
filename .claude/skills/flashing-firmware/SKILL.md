---
name: flashing-firmware
description: Use when flashing Clauled to the ESP32-C3 or setting its credentials - first-time setup, changing WiFi network, rotating the push key, or reflashing after code changes
---

# Flashing Firmware

## Overview

Collects the device's three configuration secrets, writes `src/secrets.h`, builds,
uploads to the board, and verifies it actually came up. `secrets.h` is gitignored
and must stay that way.

## When to Use

- "flash the device", "set up the ESP", "change the WiFi", "rotate the push key"
- After any firmware change that needs to reach the board

## Preflight

**1. Is PlatformIO available?**

```bash
which pio || ls ~/.platformio/penv/Scripts/pio.exe
```

If missing, it must be installed before anything else — offer either
`pip install platformio` or the VS Code PlatformIO extension, and confirm which
before installing anything.

**2. Is the board connected?**

```bash
pio device list
```

Note the port. If several USB serial devices are present, list them and ask which
is the ESP rather than guessing — flashing the wrong device is worth avoiding.

## Collecting the secrets

Ask for all three together:

| Value | Notes |
|---|---|
| `WIFI_SSID` | 2.4 GHz only — the ESP32-C3 has no 5 GHz radio. A 5 GHz SSID is a common silent failure. |
| `WIFI_PASSWORD` | See the warning below |
| `CLAULED_PUSH_KEY` | Offer to generate one; only ask if they want to set it themselves |

Generate a key with:

```bash
node -e "console.log(require('crypto').randomBytes(24).toString('base64url'))"
```

> **Say this once, then drop it.** Anything typed into chat is saved in the
> conversation transcript, including the WiFi password. The user has chosen this
> tradeoff deliberately. Mention it on a first-time setup, then proceed without
> repeating it on later runs.

## Writing secrets.h

Update **only** the three values. `secrets.h` also holds display settings
(`CYCLE_TIME`, `SHOW_WEEKLY_SONNET`, `SHOW_UPTIME`, `STALE_AFTER_S`) that must
survive untouched — read the file, replace the three `#define` lines, write it
back. Do not regenerate it from the template.

If `src/secrets.h` does not exist yet:

```bash
cp src/secrets.h.example src/secrets.h
```

Then verify it is still ignored — this must never come back as tracked:

```bash
git check-ignore -v src/secrets.h
git status --short   # secrets.h must NOT appear
```

## Build and upload

```bash
pio run
```

A blank required value fails here with a readable `static_assert` message. That is
the guard working, not a bug — go back and fill it in.

```bash
pio run --target upload
```

If auto-detection picks the wrong port, pass it explicitly:

```bash
pio run --target upload --upload-port COM7
```

**If upload fails to connect:** this board uses native USB CDC
(`ARDUINO_USB_MODE=1`, `CDC_ON_BOOT=1`) and may need manual download mode — hold
BOOT, tap RESET, release BOOT. That is physical, so the user has to do it. Ask
them to, then retry.

## Verify — do not skip

A successful upload is not proof it works. Check all three:

**1. Serial boot log** (run with a timeout; the monitor does not exit on its own)

```bash
pio device monitor --baud 115200
```

Expect `[boot] Clauled v...`, then `[wifi] connected, IP=...`, then
`[http] listening on :80`.

**2. It answers over the network**

```bash
curl http://clauled.local/health
```

Returns version, uptime, and `last_push_age: -1` before any push. If
`clauled.local` does not resolve, try the IP from the boot log — some networks
block mDNS across segments.

**3. It renders a real push**

```bash
curl -X POST http://clauled.local/push \
  -H "X-Clauled-Key: <key>" -H "Content-Type: application/json" \
  -d '{"v":1,"usage":{"five_hour":{"pct":23.5,"resets_in":4920}}}'
```

The OLED should leave "Waiting for data" and draw the bar. Confirm the user sees
it before calling the flash done.

## Common Mistakes

| Mistake | Fix |
|---|---|
| Regenerating `secrets.h` from the template | Wipes display settings — edit the three lines in place |
| Reporting success on upload alone | Upload only proves bytes moved; verify `/health` and a real push |
| Assuming a 5 GHz network will work | C3 is 2.4 GHz only |
| Committing `secrets.h` | Check `git status` after writing it, every time |
| Changing the push key and forgetting the pusher | The desktop side needs the new key too, or every push 401s |
