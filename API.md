# Clauled serial protocol

The device is a display, not a client. It has no network stack, no credentials
and no endpoints. It reads newline-delimited JSON from USB serial and renders it
on one screen.

The protocol carries **labelled display fields**, not fixed metrics — the host
decides what each gauge means. Changing the data source needs no firmware change.

Transport: **USB CDC serial**. Baud rate is irrelevant; the ESP32-C3 uses native
USB, so any value works.

Finding the device: it enumerates with Espressif's USB vendor ID **`303A`**.
Match on that rather than hardcoding a port, and it survives being moved.

## Sending data

One JSON object per line, terminated with `\n`:

```json
{"v":3,"title":"Opus 5 med","gauge1":{"label":"5h session","pct":23},"gauge2":{"label":"Context","pct":74},"row":{"left":"1h21m","right":"743k/1M"},"footer":{"left":"$0.00"}}
```

| Field | Type | Renders as |
|---|---|---|
| `v` | int | Schema version, currently `3`. A mismatch is rejected, never guessed at. |
| `title` | string | Header, right side — model and effort |
| `gauge1` / `gauge2` | object | `{ "label": string, "pct": number }`. `pct` below 0 shows `--` with an empty bar. |
| `row` | object | `{ "left": string, "right": string }` — the detail line |
| `footer` | object | `{ "left": string }` — the right side is the device's own link state |
| `busy` | string | Spinner text. **Empty string clears it.** |
| `events` | array | `[{ "text": string }]` — raises an inverted banner |

Every string is truncated to **21 characters**, one screen line. `busy` should
stay under 19 to leave room for the spinner.

## Screen layout

```
Claude                Opus 5 med     title
────────────────────────────────
5h session                  23%     gauge1
██████░░░░░░░░░░░░░░░░░░░░░░░░░░
Context                     74%     gauge2
████████████████████░░░░░░░░░░░░
1h21m                   743k/1M     row
────────────────────────────────
$0.00                    USB ok     footer
```

The detail row resolves in priority order:

1. **`events` banner** — inverted block, always wins
2. **`busy` spinner** — animates on the device at ~3 Hz
3. **`row`** — the static detail

## Replies

Every line is answered with a single JSON object:

| Reply | Meaning |
|---|---|
| `{"ok":true}` | Accepted |
| `{"error":"bad JSON"}` | Line did not parse |
| `{"error":"unsupported schema version"}` | `v` is not 3 |
| `{"error":"line too long"}` | Over 2048 bytes |

**Log output is distinguishable from replies.** Human-readable logging is
prefixed with `# `; protocol replies always begin with `{`. Ignore any line that
does not start with `{`.

## Status probe

```json
{"v":3,"cmd":"status"}
```

```json
{"ok":true,"version":"3.0.0","display_ok":true,"uptime":141,"last_push_age":114,"schema":3}
```

`display_ok` is meaningful only for I2C modules. **SPI has no acknowledgement,
so it always reports `true`** — only pixels confirm a working SPI display.

Use this to confirm a port really is a Clauled device before pushing to it.

## Semantics that matter

**Everything merges.** A hook pushing only `busy` must not wipe the gauges, and
the statusline pushing only gauges must not clear the spinner. Send partial
payloads freely — that is the intended usage.

**Gauges are independent.** One feed failing leaves its gauge at `--` while the
other keeps working. Never blank the screen because one source is unavailable.

**The device animates by itself.** The spinner and the sleep animation run on
device time, so a long turn keeps moving with no pushes at all.

**Busy self-expires** after 3 minutes, so a missed `Stop` hook cannot leave the
spinner running forever. Banners expire after 5 minutes. The BOOT button clears
either immediately.

**Sleep is automatic.** No push for `STALE_AFTER_S` (default 300s) and the
device switches to the sleep animation by itself. The host does not signal it —
a sleeping PC cannot.

## Example

Node, write-only — no native `serialport` dependency needed:

```js
import { openSync, writeSync, closeSync } from 'node:fs';
const fd = openSync('\\\\.\\COM8', 'w');           // Windows device path
writeSync(fd, JSON.stringify({ v: 3, busy: 'Running Bash' }) + '\n');
closeSync(fd);
```

On Linux and macOS the path is `/dev/serial/by-id/…`, `/dev/ttyACM0` or
`/dev/cu.usbmodem*`.

PowerShell, round trip — hold DTR/RTS low or opening the port resets the board:

```powershell
$p = New-Object System.IO.Ports.SerialPort 'COM8',115200,'None',8,'one'
$p.DtrEnable = $false; $p.RtsEnable = $false
$p.NewLine = "`n"; $p.Open(); Start-Sleep -Milliseconds 400; $p.DiscardInBuffer()
$p.WriteLine('{"v":3,"cmd":"status"}')
Start-Sleep -Milliseconds 700; $p.ReadExisting(); $p.Close()
```

## Notes for the pusher

**Context occupancy** is computed from the session transcript — sum
`input_tokens`, `cache_read_input_tokens` and `cache_creation_input_tokens` from
the newest `message.usage` block, divided by `context_window_size`. The
`context_window` figures in the statusline payload itself read zero and are not
usable.

**Subscription limits are not available locally.** Claude Code's statusline
payload carries no `rate_limits`, and neither does the transcript. The only
source is the `anthropic-ratelimit-unified-5h-*` response headers, which require
an authenticated API call. Without one, send `gauge1.pct: -1`.
