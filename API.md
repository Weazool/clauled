# Clauled serial protocol

The device is a display, not a network client. It has no WiFi stack, no
credentials and no endpoints. It reads newline-delimited JSON from USB serial
and renders it.

Transport: **USB CDC serial**. The baud rate is irrelevant — the ESP32-C3 uses
native USB, so any value works.

Finding the device: it enumerates with Espressif's USB vendor ID **`303A`**.
Match on that rather than hardcoding a port, and it survives being moved to a
different socket.

## Sending data

Write one JSON object per line, terminated with `\n`:

```json
{"v":1,"usage":{"five_hour":{"pct":23.5,"resets_in":4920},"seven_day":{"pct":41.2,"resets_in":340000}},"events":[{"type":"attention","text":"Claude needs input"}]}
```

| Field | Type | Notes |
|---|---|---|
| `v` | int | Schema version, currently `1`. A mismatch is rejected, never guessed at. |
| `usage.*.pct` | float | 0–100, percent **used**. |
| `usage.*.resets_in` | int | **Seconds remaining**, not a timestamp. |
| `events[].type` | string | Short label, shown as the page title. Truncated to 21 chars. |
| `events[].text` | string | Body text, wrapped over 2 lines. Truncated to 40 chars. |

Recognised usage buckets: `five_hour`, `seven_day`, `seven_day_sonnet`.

## Replies

The device answers every line with a single JSON object on one line:

| Reply | Meaning |
|---|---|
| `{"ok":true}` | Accepted |
| `{"error":"bad JSON"}` | Line did not parse |
| `{"error":"unsupported schema version"}` | `v` is not 1 |
| `{"error":"line too long"}` | Over 2048 bytes |

**Log output is distinguishable from protocol replies.** Human-readable logging
is prefixed with `# `; protocol replies always begin with `{`. Ignore any line
that does not start with `{`.

## Status probe

```json
{"v":1,"cmd":"status"}
```

```json
{"ok":true,"version":"2.0.0","display_ok":true,"uptime":141,"last_push_age":114,"schema":1}
```

| Field | Meaning |
|---|---|
| `version` | Firmware version, so you can tell what is actually flashed |
| `display_ok` | Whether the display initialised. **In SPI mode this is always `true`** — SPI has no acknowledgement, so only pixels confirm a working display. Meaningful only for I2C modules. |
| `last_push_age` | Seconds since the last accepted push, or `-1` if there has never been one |

Use this to confirm a port really is a Clauled device before pushing to it.

## Semantics that matter

**Everything is optional and pushes merge.** A push containing only `events`
leaves the usage bars intact, and vice versa. Send partial payloads freely.

**`resets_in` is seconds, deliberately.** The device has no wall clock — no NTP,
no timezone handling. It stamps the arrival time and counts down locally, so the
display stays accurate between pushes. Your pusher does the `resets_at - now`
subtraction.

**Staleness is not an error.** The device tracks time since the last accepted
push and marks it stale past `STALE_AFTER_S` (default 300s). Since the pusher
only runs while Claude Code is open, an overnight gap is expected and reads as
information rather than a fault.

**Events expire.** The most recent event gets its own page for `EVENT_TTL_S`
(default 300s), then the page disappears. Only the last event in an array is
retained; at most 8 are parsed per line.

## Example

PowerShell, round trip:

```powershell
$p = New-Object System.IO.Ports.SerialPort 'COM8',115200,'None',8,'one'
$p.NewLine = "`n"; $p.Open(); Start-Sleep -Seconds 3; $p.DiscardInBuffer()
$p.WriteLine('{"v":1,"usage":{"five_hour":{"pct":23.5,"resets_in":4920}}}')
Start-Sleep -Milliseconds 500; $p.ReadExisting(); $p.Close()
```

Node, write-only — no native `serialport` dependency needed:

```js
import { openSync, writeSync, closeSync } from 'node:fs';
const fd = openSync('\\\\.\\COM8', 'w');           // Windows device path
writeSync(fd, JSON.stringify({ v: 1, usage: { five_hour: { pct: 23.5, resets_in: 4920 } } }) + '\n');
closeSync(fd);
```

On Linux and macOS the path is `/dev/serial/by-id/…` or `/dev/ttyACM0` /
`/dev/cu.usbmodem*`.

## Notes for the pusher

The intended source is a Claude Code statusline script, which receives a
`rate_limits` object on stdin containing the 5h and 7d percentages — no token
required anywhere in the system. Hooks do **not** receive rate-limit data, so
events (`Stop`, `Notification`) and usage come from different mechanisms.

`resets_at` in that payload is an absolute epoch; convert to seconds-remaining
before sending.
