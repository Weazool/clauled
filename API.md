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
{"v":3,"sid":"a1b2c3d4","session":"clauled-pusher","title":"Sonnet 5","quiet":false,"gauge1":{"label":"5h","pct":55},"gauge3":{"label":"1w","pct":21,"reset":"20h49m"},"gauge2":{"label":"ctx","pct":45},"row":{"left":"4h33m","right":"357k/1M"},"footer":{"right":"xhigh"}}
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

**Which session is shown rotates every 6 seconds (`ROTATE_INTERVAL_S`)** —
flat round-robin through every active session in roster order, regardless of
whether it needs attention, is working, or is idle. Up to v3.7.0 this was
picked by priority (attention beats working beats idle), but that let one
stuck banner monopolise the screen for as long as it stayed stuck, hiding
every other session in the meantime; cycling through everyone means you
always see the rest too. The status row for whichever slot is showing still
tells you its own state — see **Screen layout** below.

If the currently-shown session drops out of the roster entirely — pruned, or
explicitly removed, see **Removing a session** below — the device snaps to
the next one immediately rather than waiting for the next tick.

**A slot only exists if a push actually carried something session-scoped** —
`title`, `session`, `gauge2`, `row.right`, `footer.right`, `busy`, or
`events`. A push with none of that — pure account-level data, e.g. a
correction to `gauge1`/`gauge3`/`quiet` alone — updates those globals and
never touches the roster. And **a slot that goes 2 minutes
(`UNCONFIRMED_GONE_S`) without ever showing a `title` or real `gauge2`** is
dropped early, well before the normal 15-minute `SESSION_GONE_S` — a genuine
session reliably picks up at least one of those within its first completed
turn, so a slot with neither after two minutes is far more likely a stray or
malformed push than a session just being slow to report.

**The header's right side is `N/M`** — position among *every* active session,
not just the ones currently being rotated. Two sessions needing attention out
of five total shows `"2/5"` then `"4/5"`, not `"1/2"` then `"2/2"` — the
number always tells you the true scale.

**Once every session has aged out, the panel does not go blank — but it does
strip back.** The account quota (`gauge1`/`gauge3`) survives the roster
emptying; it was never session data. So the idle screen is exactly three
things: the header `"Idle"`, row 1 still alternating, and a sleep animation
filling everything below it.

Nothing else is drawn — no context row, no status row, no footer rule, and
**no model**. Every one of those describes a session, and there is no session
to describe; a model name left sitting under an empty roster is stating
something that is no longer true. `title` is still remembered, and reappears
the moment a session does.

Nothing having ever been pushed at all shows the original `"Waiting for
data"` screen instead — that is a different state from idle.

**Whenever a session IS on screen, the model always shows in the footer, even
for one that has not reported its own yet.** Hook payloads never carry the
model (see Notes), so a session whose first-ever push is a hook would
otherwise show a blank left corner until its own statusline eventually fires.
It falls back to the last model seen from *any* session rather than show
nothing. (The idle screen has no footer at all — see above.)

`{"v":3,"cmd":"status"}` reports the current roster size as `sessions`, and a
per-slot breakdown as `roster` — see **Status probe** below.

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

1. **`events` banner** — the header AND this row invert while it is live, not
   the gauges — see below. Always wins.
2. **`busy` spinner** — animates on the device at ~3 Hz
3. **sleep** — automatic after `STALE_AFTER_S`, not host-driven
4. **nothing** — a legitimate state: the last turn ended over 5 minutes ago but
   the host is still pushing

**A live `events` banner inverts the header and this row — white background,
black text — leaving the gauges and footer in their normal colours.** This is
drawn in software: a filled rectangle in the inverse colour behind normally-
drawn text, confined to the two rows actually about the banner. Up to v3.7.0
this used `invertDisplay(true)`, a single controller command
(`SH110X_INVERTDISPLAY`) that flips every pixel's polarity in hardware — it
touched the whole panel, including both gauges, which turned out to be a
louder signal than the banner needed.

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
{"ok":true,"version":"3.9.1","display_ok":true,"uptime":141,"last_push_age":114,"quiet_sleep":false,"sessions":2,"roster":[{"sid":"a1b2c3d4","name":"clauled-pusher","age":3,"busy":"Running Bash"},{"sid":"e5f6a7b8","name":"other-project","age":47,"event":"Your turn"}],"quota":{"5h":57,"1w":34,"alternating":true,"showing":"1w"},"schema":3}
```

`display_ok` is meaningful only for I2C modules. **SPI has no acknowledgement,
so it always reports `true`** — only pixels confirm a working SPI display.

`quiet_sleep` is `true` while the panel is powered off for quiet hours. Check
this before assuming an unresponsive-looking device is broken at 2am.

`sessions` is how many are currently in the roster — 0 either means nothing
has ever been pushed, or every session has aged out; `last_push_age` tells
you which (`-1` only in the former case).

`roster` (v3.8.0+) is a breakdown of every slot: `sid`, `name`, `age` (seconds
since its last push), and `event`/`busy` when either is currently live —
omitted, not sent empty, otherwise. Without this, `"sessions":3` alone gives
no way to tell a lingering, about-to-be-pruned entry from a genuinely new
one; with it, a surprising count is a one-line lookup instead of a guess.

`quota` (v3.9.0+) is row 1's state: both readings (`-1` when never received),
whether it is `alternating`, and which of the two it is `showing` this
instant. `alternating` is false exactly when no weekly reading has arrived,
which is the only real reason row 1 ever looks stuck — and on the glass that
is indistinguishable from a broken rotation, so the device says which.

Use this to confirm a port really is a Clauled device before pushing to it.

## Removing a session

```json
{"v":3,"cmd":"forget","sid":"a1b2c3d4"}
```

Drops that one slot from the roster immediately (v3.8.0+), rather than
waiting for `SESSION_GONE_S` (15 min) or `UNCONFIRMED_GONE_S` (2 min) to age
it out on its own. Replies `{"ok":true}` whether or not a session with that
`sid` was actually present — the caller does not need to know in advance.

This exists for tools that create a session deliberately for a one-off test.
`doctor.mjs`'s diagnostic push uses a dedicated `sid` (`doctor00`)
specifically so it never collides with a real session, and now calls
`forget` on it immediately after restoring the real values, instead of
leaving it in the roster inflating the count for a quarter of an hour after
every single run.

**Firing several pushes back-to-back with no gap is not reliable** — each
accepted line makes the device parse JSON and redraw the whole panel (an SPI
transfer) before it reads more serial, and this can outrun a burst of pushes
sent with no pacing, silently dropping the tail of the burst. Confirmed on
real hardware, not theoretical. A normal hook only ever sends one push at a
time, so this does not matter there; anything that fires several in a row —
like a test/restore/forget sequence — should pace them with a short delay
(100–150 ms is comfortably enough) between calls.

## Writing a line safely

Two rules, both learned the hard way — a write that returns success is **not**
proof the device received the line.

**Start every line with a newline.** The device accumulates bytes until it
sees one. Anything that leaves an unterminated fragment in its buffer — a
short write, a retried write, or two concurrent writers interleaving on the
one port — means your next line is appended to that fragment and lost. A
leading newline terminates the fragment so your line always starts clean. One
byte, and it makes each push independent of whether the last one was clean.

**Pace lines longer than ~200 bytes.** Firmware before v3.9.0 has a 256-byte
USB CDC receive queue (the arduino-esp32 default), which is *smaller than a
full display push*. Measured on real hardware against v3.8.0: lines up to 261
bytes arrived intact, lines of 301 bytes and up were truncated — and a
truncated line has no newline, so it poisons the buffer for everything after
it. Write long lines in chunks of ~128 bytes with a few milliseconds between
them. v3.9.0+ presets its buffer to `LINE_MAX` and no longer needs this, but
pacing costs almost nothing and keeps a host working against older firmware.

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

**Busy self-expires** after `BUSY_TTL_S` (default 90s) per session, so a
missed `Stop` hook cannot leave that one spinning forever. Banners expire
after `EVENT_TTL_S` (default 300s). The
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
`resets_at` matches the header epoch exactly. **The headers cover BOTH
figures** — `anthropic-ratelimit-unified-5h-*` and
`anthropic-ratelimit-unified-7d-*` arrive in the same response, so one
authenticated call supplies the weekly reading as well as the 5h one. (This
document previously claimed no weekly header existed. It does; believing
otherwise left the weekly gauge permanently blank on any setup relying on
the token path.)

**A reset countdown (`row.left`, `gauge3.reset`) is always formatted as hours
and minutes, never days** — `"76h23m"`, not `"3d4h"`. Multi-day countdowns
just produce a large hour count. A device or pusher that ever displays a
day-formatted string is showing a hardcoded literal, not a real reading.

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
