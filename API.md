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
{"v":3,"session":"clauled-pusher","title":"Sonnet 5","quiet":false,"gauge1":{"label":"5h reset","pct":55},"gauge2":{"label":"ctx","pct":45},"row":{"left":"4h33m","right":"357k/1M"},"footer":{"right":"xhigh"}}
```

| Field | Type | Renders as |
|---|---|---|
| `v` | int | Schema version, currently `3`. A mismatch is rejected, never guessed at. |
| `session` | string | Header — which session, centred |
| `title` | string | Footer, left — the model |
| `quiet` | bool | Not rendered directly — see Quiet hours below |
| `gauge1` / `gauge2` | object | `{ "label": string, "pct": number }`. `pct` below 0 shows `--` with an empty bar. |
| `row` | object | `{ "left": string, "right": string }` — `left` pairs with `gauge1`, `right` with `gauge2` |
| `footer` | object | `{ "right": string }` — the effort level. `left` is accepted but ignored — see Notes. |
| `busy` | string | Spinner text. **Empty string clears it.** |
| `events` | array | `[{ "text": string }]` — raises an inverted banner |

`session` and `footer.right` were added in firmware v3.3.0; `title` moved from
the header to the footer in v3.4.0, and `footer.left` (cost) stopped being
drawn there; `quiet` was added in v3.5.0. All of these are additive or
render-only — the field names already established keep their meaning, so a
v3.2.0-or-later pusher still renders correctly with no change, and any pusher
that never sends `quiet` simply never triggers the quiet-hours power-down.

Every string is truncated to **21 characters**, one screen line. `busy` should
stay under 19 to leave room for the spinner.

## Screen layout

```
      clauled-pusher                session, centred
────────────────────────────────
5h reset      4h33m        55%      gauge1.label | row.left | gauge1.pct
███████████████████░░░░░░░░░░░░░    gauge1.pct
ctx         357k/1M        45%      gauge2.label | row.right | gauge2.pct
███████████████░░░░░░░░░░░░░░░░░    gauge2.pct
/ Running Bash                      busy / events / sleep
────────────────────────────────
Sonnet 5                 xhigh      title | footer.right
```

**Each data row is three columns**: the gauge label flush left, its paired `row`
side centred, the percentage flush right. The device places them — the host just
supplies the three strings.

The middle is centred on the **screen**, not on the gap, so it does not shift
when the percentage widens. It moves only to avoid a collision, always keeping
one blank character on each side, and is dropped entirely if even that will not
fit. The label and the percentage are never sacrificed.

Keep labels short. `5h reset` (8 characters) is about the practical limit before
the middle column is pushed off centre on every render.

The status row resolves in priority order:

1. **`events` banner** — inverted block, always wins
2. **`busy` spinner** — animates on the device at ~3 Hz
3. **sleep** — automatic after `STALE_AFTER_S`, not host-driven
4. **nothing** — a legitimate state: the last turn ended over 5 minutes ago but
   the host is still pushing

Above all of that, and independent of it: if `quiet` is `true` and idle exceeds
`QUIET_IDLE_S` (default 900s / 15 min), the panel is powered off entirely —
`SH110X_DISPLAYOFF` sent once on the transition, nothing drawn while off. See
**Quiet hours** below.

## Quiet hours

The device has no clock and never will — that is why NTP was removed outright
in v2.0.0. Whether it is currently "quiet hours" is a **host** decision, sent
on every push as a plain boolean:

```json
{"v":3,"quiet":true}
```

The device only owns the OTHER half: how long is idle. `quiet:true` plus more
than `QUIET_IDLE_S` since the last push powers the panel off. Any push at all
wakes it — idle resets to 0 on every push, whether or not that push mentions
`quiet` — so the device is never dark while data is actually arriving.

**Send `quiet` on every push, true or false, never omitted.** The device only
updates it on an explicit value; an omitted key leaves the last one in place.
Send it once as `true` and stop sending it, and the panel stays dark forever
once idle, even long after quiet hours end.

**This can only ever be as fresh as the last push.** If the host goes
completely silent for hours — Claude Code fully closed — spanning a quiet-hours
boundary, the device is working from whatever `quiet` value the last push
carried, however old. This is the same tradeoff already accepted for the 5h
countdown, which also just decrements locally between pushes with no resync:
the price of no network stack and no wall clock.

The BOOT button bypasses the power-down for exactly one frame — a brief flash
to prove the device is alive, not a sustained wake. The very next tick
re-applies the policy and, since a button press does not reset the idle timer,
puts it straight back to sleep.

`{"v":3,"cmd":"status"}` reports the current state as `quiet_sleep` — see
Status probe below. A dark panel with `quiet_sleep:true` is working as
intended, not a fault.

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
{"ok":true,"version":"3.5.0","display_ok":true,"uptime":141,"last_push_age":114,"quiet_sleep":false,"schema":3}
```

`display_ok` is meaningful only for I2C modules. **SPI has no acknowledgement,
so it always reports `true`** — only pixels confirm a working SPI display.

`quiet_sleep` is `true` while the panel is powered off for quiet hours. Check
this before assuming an unresponsive-looking device is broken at 2am.

Use this to confirm a port really is a Clauled device before pushing to it.

## Semantics that matter

**Everything merges.** A hook pushing only `busy` must not wipe the gauges, and
the statusline pushing only gauges must not clear the spinner. Send partial
payloads freely — that is the intended usage.

**Gauges are independent.** One feed failing leaves its gauge alone while the
other keeps working. Never blank the screen because one source is unavailable.

**The device animates by itself.** The spinner and the sleep animation run on
device time, so a long turn keeps moving with no pushes at all.

**Sleep does not take the screen.** It renders in the status row only, so the
gauges stay readable while the host is away.

**Busy self-expires** after 3 minutes, so a missed `Stop` hook cannot leave the
spinner running forever. Banners expire after 5 minutes. The BOOT button clears
either immediately.

**Sleep is automatic.** No push for `STALE_AFTER_S` (default 300s) and the
device switches to the sleep animation by itself. The host does not signal it —
a sleeping PC cannot.

## Example

Node, write-only — no native `serialport` dependency needed:

```js
import { openSync, writeSync, closeSync, constants as C } from 'node:fs';
// NOT 'w'. That is O_WRONLY|O_CREAT|O_TRUNC, so a wrong path does not fail -
// it creates a regular file and every write silently "succeeds" into it.
const fd = openSync('\\\\.\\COM8', C.O_WRONLY);    // Windows device path
writeSync(fd, JSON.stringify({ v: 3, busy: 'Running Bash' }) + '\n');
closeSync(fd);
```

On Linux the path is `/dev/serial/by-id/…` or `/dev/ttyACM0`. On macOS it is
`/dev/cu.usbmodem*` — **always `cu.`, never `tty.`**. The `tty.` node is the
dial-in side and `open()` on it blocks until carrier is asserted, which hangs
the caller rather than failing. Open with `O_NOCTTY | O_NONBLOCK` there.

PowerShell, round trip — hold DTR/RTS low or opening the port resets the board:

```powershell
$p = New-Object System.IO.Ports.SerialPort 'COM8',115200,'None',8,'one'
$p.DtrEnable = $false; $p.RtsEnable = $false
$p.NewLine = "`n"; $p.Open(); Start-Sleep -Milliseconds 400; $p.DiscardInBuffer()
$p.WriteLine('{"v":3,"cmd":"status"}')
Start-Sleep -Milliseconds 700; $p.ReadExisting(); $p.Close()
```

## Notes for the pusher

**The statusline payload varies per invocation.** Anywhere from 16 keys down to
`{model, effort}`. Nothing may assume a field is present, and the two notes
below were originally written from a sample that happened to contain only the
reduced payloads — both conclusions were wrong.

**Subscription limits ARE available locally**, in the payload's `rate_limits`
block, with `five_hour` and `seven_day` each carrying `used_percentage` and
`resets_at`. No credential is needed. It is not sent on every invocation, so
cache the last reading and carry it forward. The
`anthropic-ratelimit-unified-5h-*` response headers give the same figures and
are now only a fallback for hosts that never send the block — a payload's
`resets_at` matches the header epoch exactly.

**Context occupancy** comes from the payload's `context_window` block when
present — `context_window_size` with a `current_usage` breakdown. When it is
absent, compute it from the session transcript: sum `input_tokens`,
`cache_read_input_tokens` and `cache_creation_input_tokens` from the newest
`message.usage` block.

**Omit what you cannot compute; never send a placeholder.** The device merges,
so a field you leave out keeps its last good value, while `pct: -1` actively
blanks the gauge to `--`. Since the payload can arrive reduced, sending `-1` for
a missing feed overwrites a good reading with nothing. Reserve `-1` for a feed
you know to be unavailable.

**Every push should compute everything it can, not just its own reason for
pushing.** A hook that only sends the field it exists for — a spinner, a
banner — leaves everything else showing whatever the last push happened to
carry, which can be minutes stale. If effort or the model changes mid-session,
that staleness is exactly what you would notice first. Recompute the full
display on every push from whatever the trigger's payload provides, merging in
only the field that push exists to add. The one field this cannot fix is the
model on a hook-only turn: Claude Code's hook payloads never carry it, only the
statusline does, so it is unavoidably cached rather than live there.
