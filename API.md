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
{"v":3,"sid":"a1b2c3d4","session":"clauled-pusher","title":"Sonnet 5","quiet":false,"gauge1":{"label":"5h","pct":55},"gauge3":{"label":"1w","pct":21,"reset":"3d4h"},"gauge2":{"label":"ctx","pct":45},"row":{"left":"4h33m","right":"357k/1M"},"footer":{"right":"xhigh"}}
```

| Field | Type | Scope | Renders as |
|---|---|---|---|
| `v` | int | — | Schema version, currently `3`. A mismatch is rejected, never guessed at. |
| `sid` | string | key | Which session this push belongs to — see **Multiple sessions** below |
| `session` | string | per-session | Header, left |
| `title` | string | per-session | Footer, left — the model |
| `quiet` | bool | global | Not rendered directly — see **Quiet hours** below |
| `gauge1` / `row.left` | object / string | global | Row 1, alternating with `gauge3` — see **Multiple sessions** |
| `gauge3` | object | global | `{ "label", "pct", "reset" }` — the other half of row 1's alternation |
| `gauge2` / `row.right` | object / string | per-session | Row 2 — context occupancy |
| `footer` | object | per-session | `{ "right": string }` — the effort level. `left` is accepted but ignored — see Notes. |
| `busy` | string | per-session | Spinner text. **Empty string clears it.** |
| `events` | array | per-session | `[{ "text": string }]` — raises an inverted banner |

**"global" fields are the same value regardless of which session's push
carries them** — the account quota does not belong to any one session, so the
last push to set it wins, full stop, no merge-per-session. **"per-session"
fields are tracked separately per `sid`** — see below.

`sid`, `gauge3` and the per-session scoping were added in v3.7.0; `quiet` in
v3.5.0; `session`/`footer.right` in v3.3.0. All additive or render-only — a
v3.2.0-or-later pusher that never sends `sid` still renders correctly, landing
everything in the device's one shared fallback slot exactly as it always did.

Every string is truncated to **21 characters**, one screen line. `busy` should
stay under 19 to leave room for the spinner.

## Multiple sessions

The device tracks up to **8 concurrent sessions**, one per distinct `sid`,
and decides on its own which one to show — this needs no host-side
coordination beyond tagging each push with the session it came from.

**A session with no push in 15 minutes (`SESSION_GONE_S`) is dropped.** A
push with no `sid` at all lands in a shared fallback slot, which is exactly
the pre-v3.7.0 single-session behaviour — nothing breaks for a pusher that
has not been updated.

**Which session is shown rotates every 3 seconds (`ROTATE_INTERVAL_S`)**,
picked by priority — the same "alert always wins" rule a single session
already had, now deciding which SESSION gets shown, not only what one
session's status row displays:

1. Any session with a live `events` banner — rotate among those, ignore the rest
2. Else any session with a live `busy` spinner — rotate among those
3. Else every remaining session — rotate among all of them

If the active group changes — a session gets a new banner, or the one you
were looking at falls out of it — the device snaps to the new group
immediately rather than waiting for the next tick.

**The header's right side is `N/M`** — position among *every* active session,
not just the ones currently being rotated. Two sessions needing attention out
of five total shows `"2/5"` then `"4/5"`, not `"1/2"` then `"2/2"` — the
number always tells you the true scale.

**Once every session has aged out, the panel does not go blank.** The account
quota (`gauge1`/`gauge3`) and the last known model survive the roster
emptying — they were never session data to begin with — so the header reads
`"Idle"`, row 1 keeps alternating exactly as before, and a graphical idle
indicator fills the lower half of the screen. Nothing has ever been pushed at
all shows the original `"Waiting for data"` screen instead — those are two
different states.

**The model always shows in the footer, even for a session that has not
reported its own yet.** Hook payloads never carry the model (see Notes), so a
session whose first-ever push is a hook would otherwise show a blank left
corner until its own statusline eventually fires. It falls back to the last
model seen from *any* session rather than show nothing.

`{"v":3,"cmd":"status"}` reports the current roster size as `sessions` — see
**Status probe** below.

## Screen layout

```
clauled-pusher                 2/5      session | N/M
────────────────────────────────
5h           4h33m        55%           gauge1.label | row.left | gauge1.pct
███████████████████░░░░░░░░░░░░░        gauge1.pct   (alternates with gauge3
ctx         357k/1M        45%           every ROTATE_INTERVAL_S)
███████████████░░░░░░░░░░░░░░░░░    gauge2.pct
/ Running Bash                      busy / events / sleep
────────────────────────────────
Sonnet 5                 xhigh      title | footer.right
```

**Each data row is three columns**: the gauge label flush left, its paired `row`
(or, for `gauge3`, its own bundled `reset`) centred, the percentage flush
right. The device places them — the host just supplies the strings.

The middle is centred on the **screen**, not on the gap, so it does not shift
when the percentage widens. It moves only to avoid a collision, always keeping
one blank character on each side, and is dropped entirely if even that will not
fit. The label and the percentage are never sacrificed.

Keep labels short. `5h` and `1w` (2 characters) are the shipped examples —
`5h reset` (8) is about the practical limit before the middle column is pushed
off centre on every render.

The status row resolves in priority order:

1. **`events` banner** — the WHOLE PANEL inverts while this is live, not just
   this row — see below. Always wins.
2. **`busy` spinner** — animates on the device at ~3 Hz
3. **sleep** — automatic after `STALE_AFTER_S`, not host-driven
4. **nothing** — a legitimate state: the last turn ended over 5 minutes ago but
   the host is still pushing

**A live `events` banner inverts every line on the screen, not just its own
row.** `invertDisplay(true)` is a single controller command
(`SH110X_INVERTDISPLAY`) that flips every pixel's polarity in hardware — it
does not touch the framebuffer, so the header, both gauges, the bars and the
footer all come out inverted along with the banner text, for free. Sent once
on the transition into and out of the state, not on every redraw. This is
deliberately the loudest signal the device has: the whole screen, not one row.

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
{"ok":true,"version":"3.7.0","display_ok":true,"uptime":141,"last_push_age":114,"quiet_sleep":false,"sessions":2,"schema":3}
```

`display_ok` is meaningful only for I2C modules. **SPI has no acknowledgement,
so it always reports `true`** — only pixels confirm a working SPI display.

`quiet_sleep` is `true` while the panel is powered off for quiet hours. Check
this before assuming an unresponsive-looking device is broken at 2am.

`sessions` is how many are currently in the roster — 0 either means nothing
has ever been pushed, or every session has aged out; `last_push_age` tells
you which (`-1` only in the former case).

Use this to confirm a port really is a Clauled device before pushing to it.

## Semantics that matter

**Everything merges — per session for per-session fields, globally for global
ones.** A hook pushing only `busy` must not wipe that session's gauges, and
the statusline pushing only gauges must not clear its busy state. `gauge1`/
`gauge3`/`quiet`, being global, merge against the single shared copy
regardless of which session's push carries them. Send partial payloads
freely — that is the intended usage.

**Gauges are independent.** One feed failing leaves its gauge alone while the
other keeps working. Never blank the screen because one source is unavailable.

**The device animates by itself.** The spinner, the sleep row, the rotation
between sessions and the quota row's own 5h/1w alternation all run on device
time — a long turn, or a quiet stretch with several sessions open, keeps
moving with no pushes at all.

**A quiet session's sleep row does not take the screen.** It renders in the
status row only, when that particular session is the one being shown, so the
gauges stay readable and other sessions keep rotating normally.

**Busy self-expires** after 3 minutes per session, so a missed `Stop` hook
cannot leave that one spinning forever. Banners expire after 5 minutes. The
BOOT button clears whichever session is *currently on screen* — not the whole
roster — so acknowledging one session's banner does not silently dismiss a
different session's "Your turn" you have not seen yet.

**A session goes quiet automatically.** No push for `STALE_AFTER_S` (default
300s) and that session's status row switches to the sleep animation on its
own. After `SESSION_GONE_S` (default 900s) with still nothing, it is dropped
from the roster entirely — see **Multiple sessions** above.

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

**`sid` should key off `session_id`**, present in both statusline and hook
payloads. A short prefix (8 hex characters is plenty) is enough — the device
only needs it to be stable and distinct per session, not globally unique.

**If you cache anything per-session on the host** — the model, for the
staleness reason above — **key that cache by `sid` too, not one shared file.**
Two sessions can genuinely be on different models at once; a single cache
would leak session A's model into session B's display the moment they
diverge. `gauge1`/`gauge3`/`quiet`, by contrast, are correct to cache
globally — they are the same value for every session under one login.
