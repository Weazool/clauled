# Changelog

All notable changes to this project are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **major** — breaking change to the push API or the secrets format
- **minor** — new capability, backwards compatible
- **patch** — fixes and internal changes only

## [Unreleased]

## [4.0.0] - 2026-08-14

**BREAKING.** The wire protocol is renamed so that every field says what it
actually does. Schema is now `4` and the device accepts **only** `4` — a v3
push is rejected with `{"error":"unsupported schema version","schema":4}`.
Requires clauled-pusher **4.0.0**; the two move together, and from here the
major version of both projects equals the schema version.

### Changed — breaking
- **`title` → `model`.** It carried the model but was named for a header
  deleted back in v3.1.0. v3.4.0 knowingly kept the wrong name for wire
  compatibility; this is that debt being paid.
- **`gauge1` + `row.left` → `quota5h`**, **`gauge3` → `quota7d`**,
  **`gauge2` + `row.right` → `context`.** Each metric now carries its own
  detail. There was never a third gauge — `gauge3` time-shares row 1 with
  `gauge1` — and `row` was the worst of it: one object holding two fields of
  *different scope*, its left half account-global and its right half
  per-session.
- **`footer.right` → `effort`**, and the `footer` object is gone. Its other
  half (`footer.left`, cost) has been dead since v3.4.0.
- **`events: [{type,text}]` → `banner: "..."`.** The array only ever kept its
  last element and `type` was never read by any firmware. An empty string
  clears it, exactly like `busy`.
- **`v` is now required.** It was `root["v"] | SCHEMA_VERSION`, so an absent
  version defaulted to *current* — a stale push with no `v` was accepted and
  then silently rendered nothing, every field name being unknown. That is the
  worst possible failure for a hard break, because it looks like success.
- **A new percentage drops a stale detail** on all three metrics. Only
  `gauge3` had this rule before, purely because the other two kept their
  detail in a separate object; merging removed the excuse. This is a real
  semantic tightening, not a rename — it is what stops a countdown computed
  for one reading being shown beside another.
- `EVENT_TTL_S` → `BANNER_TTL_S`, matching the field it governs.

### Added
- The rejection reply names the schema the device speaks, so a mismatched
  host is told what to be rather than only that it is wrong.

## [3.9.1] - 2026-08-14

### Changed
- **The idle screen strips back to three things**: the header `Idle`, the
  quota row still alternating, and the sleep animation across everything
  below it. The model is gone from it — v3.9.0 still drew it bottom-left.
  A model describes a *session*, and on this screen there is no session to
  describe, so leaving it there stated something no longer true. It is still
  remembered and reappears the moment a session does; the active screen is
  unchanged and still shows every field.
- The animation reclaims the row the footer occupied, so the three Z's now
  run the full height from just under the quota bar to the bottom edge.

## [3.9.0] - 2026-08-13

**Fixes the bug underneath most of the others: the device could not receive a
full-size push at all.**

### Fixed
- **The USB serial receive buffer was smaller than a display push.**
  arduino-esp32 allocates a 256-byte RX queue for USB CDC by default
  (`HWCDC::begin`: "RX Buffer default has 256 bytes if not preset"). A full
  display push is around 300 bytes. Measured on real hardware by pushing
  increasing sizes and checking which arrived: **everything up to 261 bytes
  landed, everything from 301 bytes up was lost.** The tail was dropped, so
  the line never got its newline, so it sat in the line buffer and the *next*
  push was appended to the fragment and lost too — and once the fragments
  passed `LINE_MAX` the overflow latch discarded good lines as well. A live
  round trip answered a well-formed 62-byte push with
  `{"error":"line too long"}`.

  The host saw none of this: every write genuinely succeeded. The visible
  result was a display that went minutes without updating, sessions missing
  from the roster, quota rows stuck at `--`, effort never appearing, and an
  idle screen with no readings on it — all of which had been chased as
  separate faults. `Serial.setRxBufferSize(LINE_MAX)` before `begin()`
  presets the queue, which `begin()` then leaves alone. Re-running the size
  sweep after the fix: every size from 81 to 601 bytes lands, none lost.
- **A partial line no longer poisons the ones after it.** `pumpSerial()` now
  treats a `{` arriving mid-buffer as the start of a new message and discards
  the fragment in front of it. Every message in this protocol is a JSON
  object and none contains an unescaped `{` after the first character, so
  this is unambiguous. Verified by deliberately writing a 3000-byte
  unterminated fragment and confirming the very next ordinary push still
  landed.

### Changed
- **The idle screen is quieter and more useful.** The `(-_-)` face is gone,
  the footer rule is gone, and the sleep animation is now **three** Z's -
  always exactly three, drifting up and right as they grow, on a path bounded
  clear of the quota bar above and the model line below. The quota row was
  always drawn here; it now has readings to show, because pushes carrying
  them arrive.

Also: makes row 1's state observable, and stops a finished session claiming
it is still working.

### Added
- **`quota` on the status probe** — `{"5h", "1w", "alternating", "showing"}`.
  "Row 1 is not alternating" and "row 1 is alternating but you happened to
  look twice in the same half of the cycle" are indistinguishable on the
  glass, and until now there was no way to tell them apart without filming
  the screen. `alternating` is false exactly when no weekly reading has
  reached the device, which is the real reason it ever looks stuck.
  `doctor` prints all of it.

### Changed
- **`BUSY_TTL_S` is 90s, was 180s.** This is the backstop that ends a
  spinner when the `Stop` push that should have ended it never arrived. Three
  minutes of a finished session insisting it was still working is worse than
  a spinner that stops early during a long stretch with no tool calls — the
  first states something false, the second just stops stating anything.
- **`BUSY_TTL_S` and `EVENT_TTL_S` moved to `config.h`**, next to the other
  timing knobs, instead of sitting among the protocol limits in `main.cpp`.
  They are behaviour tunables, and they now read as such.

## [3.8.0] - 2026-08-13

Four fixes and a rotation-policy change, all found and verified against the
real multi-session roster introduced in v3.7.0.

### Changed
- **A live banner inverts only the header and the status row**, not the
  whole panel. `invertDisplay()` (the hardware command used since v3.6.0)
  flipped every pixel including both gauges; this confines "it's your move"
  to the two rows actually about it, drawn in software (a filled rectangle
  behind inverse-colour text) so the gauges stay in their normal colours.
- **Session rotation is flat, every `ROTATE_INTERVAL_S`** (now 6s, was 3s) —
  every active session gets equal time in roster order, regardless of
  whether it needs attention, is working, or is idle. The v3.7.0 priority
  scheme (attention beats working beats idle) let one stuck banner
  monopolise the screen for as long as it stayed stuck; cycling through
  everyone means you always see the rest too. The quota row's 5h/1w
  alternation shares the same clock, so it is also now 6s.

### Added
- **`roster` on the status probe** — a per-slot breakdown (`sid`, `name`,
  `age`, and `event`/`busy` when live), alongside the existing `sessions`
  count. A bare count gives no way to tell a lingering entry from a
  genuinely new session; this makes a surprising count a one-line lookup.
- **`{"cmd":"forget","sid":"..."}`** — drops one session from the roster
  immediately rather than waiting `SESSION_GONE_S` (15 min) to age out.
  `doctor.mjs` now calls this on its own test session right after every run.
- **Two guardrails on what gets onto the roster, and how long it stays**,
  after a debugging session that itself produced two different phantom
  entries (see Fixed, below):
  - A push only touches the roster if it carries something session-scoped —
    `title`, `session`, `gauge2`, `row.right`, `footer.right`, `busy`, or
    `events`. A push carrying only account-level data (a correction to
    `gauge1`/`gauge3`/`quiet` alone, say) updates those globals and never
    creates or touches a slot.
  - A slot that has never shown a title or real context — `UNCONFIRMED_GONE_S`,
    2 minutes — is dropped far sooner than the normal 15-minute
    `SESSION_GONE_S`. A genuine session reliably picks up one of those within
    its first completed turn; a slot with neither after two minutes is far
    more likely a stray or malformed push.

### Fixed
- **The weekly quota could get stuck on a fake value forever.**
  `doctor.mjs`'s test push set `gauge3` to a hardcoded `{pct:61,reset:"3d4h"}`
  to prove the alternation animation, but only restored it afterward when a
  real weekly reading happened to be cached — if not, the fake value simply
  had nothing to correct it, and nothing else ever resends `gauge3` on its
  own. `pct:-1` is now a documented sentinel that hides the weekly row again
  (`hasWeek` goes false), so the restore can always leave the device honest
  even with no real value to restore to.
- **A globals-only push (no `sid`, nothing session-scoped) used to create or
  touch a roster slot anyway** — usually the shared `""` fallback slot,
  showing up as a session with no name and empty gauges. Reproduced live
  while manually correcting the account quota during this release's testing.
  See the guardrail above.

```
clauled-pusher                 2/5
────────────────────────────────
5h            4h33m        55%      <- alternates with the weekly quota,
███████████████████░░░░░░░░░░░░░       every 6s now, same as rotation
ctx         357k/1M        45%
███████████████░░░░░░░░░░░░░░░░░
/ Running Bash
────────────────────────────────
Sonnet 5                 xhigh
```

## [3.7.0] - 2026-08-13

Multiple Claude Code sessions, tracked and rotated on the device with no host
coordination beyond tagging each push with which session it came from.

```
clauled-pusher                 2/5
────────────────────────────────
5h            4h33m        55%      <- alternates with the weekly quota
███████████████████░░░░░░░░░░░░░
ctx         357k/1M        45%
███████████████░░░░░░░░░░░░░░░░░
/ Running Bash
────────────────────────────────
Sonnet 5                 xhigh
```

### Added
- **A session roster, up to 8 slots**, keyed by a new `sid` field. A push with
  no `sid` lands in a shared fallback slot - the exact pre-3.7.0 behaviour,
  unaffected for a pusher that has not been updated.
- **Rotation, every `ROTATE_INTERVAL_S` (3s)**, picked by priority: any
  session needing attention beats any session merely working beats
  everything else. The same "alert always wins" rule a single session
  already had, now deciding which SESSION is shown, not only what one
  session's status row displays. If the active group changes, the device
  snaps to it immediately rather than waiting for the next tick.
- **A per-session prune**, `SESSION_GONE_S` (default 900s / 15 min) - no push
  from a session in that long and it is assumed closed, dropped from the
  roster.
- **The header is `session` left, `N/M` right** - position among *every*
  active session, not just the ones currently rotating. Two sessions needing
  attention out of five shows `2/5` then `4/5`, not a smaller-looking `1/2`.
- **`gauge3`** - the weekly (7-day, all models) quota. Alternates with
  `gauge1` in row 1's slot, on its own `ROTATE_INTERVAL_S` clock, entirely
  independent of session rotation.
- **An `Idle` screen** for when the roster empties but the device has seen
  real data this boot: header reads `Idle`, the quota row keeps alternating,
  a bigger idle graphic (the pre-v3.1.0 growing-Z animation, which finally has
  room again) fills the lower half. Different from `Waiting for data`, which
  is for a device that has never received anything at all.
- **The footer's model never goes blank** while any session has ever reported
  one. A session whose first-ever push is a hook (hooks never carry the
  model) falls back to the last model seen from *any* session rather than
  show nothing.

### Changed
- **`gauge1`/`row.left` (5h quota) and `gauge3` are account-level, not
  per-session.** The same reading regardless of which session's push carries
  it - merge is against one shared copy, not per-slot. `gauge2`/`row.right`
  (context) and everything else stays genuinely per-session.
- **BOOT now clears only the session on screen**, not the whole roster -
  dismissing one session's banner must not silently dismiss a different
  session's "Your turn" you have not seen yet.
- **The quiet-hours idle clock (`shouldPowerDown`) now tracks every session
  at once**, not whichever happens to be displayed. One quiet session must
  not power off a panel a different session is actively using.

### Notes
- No protocol break. `sid` and `gauge3` are additive; a pusher that predates
  this release still renders correctly with no change, at v3.4.0's layout.
- Verified on real hardware, not just compiled: pushed two and then four
  distinct sessions and confirmed the roster count tracked correctly; forced
  a session to age out with a temporarily shortened `SESSION_GONE_S` and
  confirmed it actually dropped; kept the device responsive (uptime
  advancing, no reboot) across a burst of multi-session pushes. Not
  camera-verified - there is no way to confirm the actual pixel layout from
  this environment, only that the roster, rotation and pruning logic behaves
  correctly and the device never crashes or hangs.
- One genuinely useful accident during that verification: this repo's own
  Claude Code session, with clauled-pusher's hooks active, kept joining the
  roster as a real, organic session alongside the synthetic test ones -
  better validation of concurrent tracking than a purely synthetic test would
  have been.

## [3.6.1] - 2026-08-13

No functional change. README trimmed roughly by a third - the same facts,
without the design-rationale essays now that they live in this file's
history instead. That history is worth reading if you want the "why"; the
README's job is just "how do I use this."

## [3.6.0] - 2026-08-13

When it's your turn, the whole screen inverts - not just the status row.

### Changed
- **A live `events` banner now inverts every line on the panel**, not just its
  own row. `invertDisplay(true)` is a single controller command
  (`SH110X_INVERTDISPLAY`, verified against the actual `Adafruit_GrayOLED.cpp`
  source rather than assumed) that flips every pixel's polarity in hardware -
  it never touches the framebuffer, so the header, both gauges, the bars and
  the footer all come out inverted along with the banner text, for free. Sent
  once on the transition, not on every redraw, so it does not spam the bus for
  the up-to-5-minute life of a banner.
- **`drawBanner()` is gone.** It used to manually paint a white rectangle
  behind black text for just the status row - the one piece of hand-rolled
  inversion in the codebase. The banner text now draws exactly like every
  other row (plain, un-inverted in the framebuffer); the hardware invert
  handles turning it white-on-black along with everything else. Inverting that
  row's own drawing a second time on top of a whole-panel invert would have
  cancelled out and made the text disappear against its own background.
- Dropped `STATUS_Y` / `STATUS_H`, which existed only for `drawBanner()`'s
  rectangle and had no other caller once it was removed.

### Notes
- No protocol change. This is triggered by the same `events` field every
  pusher already sends - existing pushers get the new behaviour automatically,
  nothing to update on that side.
- Verified on real hardware: pushed a `stop` event, then a `notification`
  event, then cleared each with a `busy` push, confirming the device stays
  responsive (uptime advancing, status probe answering) across all four
  invert transitions. Not camera-verified - there is no way to confirm the
  actual pixels from this environment, only that the command sequence runs
  without crashing or hanging the device.

## [3.5.0] - 2026-08-13

Quiet hours. Idle for long enough during a configured overnight window, the
panel powers off entirely - not the sleep animation, an actual
`SH110X_DISPLAYOFF`.

### Added
- **`quiet`** (bool) - the host's decision, sent on every push. Combined with
  `QUIET_IDLE_S` (default 900s / 15 min) of idle time, powers the panel off
  with `SH110X_DISPLAYOFF` rather than merely dimming the sleep animation.
  Sent through `oled_command()`, which is bus-agnostic - works identically on
  I2C and SPI wiring, verified on real hardware with a temporarily shortened
  threshold rather than assumed from the datasheet.
- **`quiet_sleep`** in the status probe reply, so a dark panel at 2am reads as
  "working as intended" instead of "device unreachable."
- The BOOT button now forces exactly one frame past the power-down gate -
  proof the device is alive, not a sustained wake. The next tick re-applies
  the policy immediately, since a button press does not reset the idle timer.

### Notes
- **The device still has no clock and never will** - this is why NTP was
  removed outright in v2.0.0. `quiet` has to come from the host, which is why
  it is the one field that must be sent on every push, true or false, never
  omitted: the device only updates it on an explicit value, so an omitted key
  after a single `true` would leave the panel dark forever.
- Any push at all wakes the device, whether or not that push mentions `quiet`
  - idle resets to zero on every push, and `shouldPowerDown()` is naturally
  false the instant that happens. No special-cased wake logic was needed.
- Verified on hardware, not just compiled: `quiet:true` past the idle
  threshold powers off; any push (even one that never mentions `quiet`) wakes
  it immediately; `quiet:false` past the same idle time never powers down -
  proving the flag gates it, not idle time alone.
- `QUIET_IDLE_S` lives in `src/config.h` next to `STALE_AFTER_S` - a device-side
  constant, deliberately separate from the host-side quiet-hours window, which
  only the host can know.

## [3.4.0] - 2026-08-13

The header is just the session now, centred. Model and effort move to the
footer. Cost is gone.

```
      clauled-pusher
────────────────────────────────
5h reset      2h44m        15%
█████░░░░░░░░░░░░░░░░░░░░░░░░░░░
ctx         357k/1M        45%
██████████████░░░░░░░░░░░░░░░░░░
/ Running Bash
────────────────────────────────
Sonnet 5                 xhigh
```

### Changed
- **Header is `session` alone, centred.** It no longer shares the row with the
  model - the header's one job now is telling you which work this is.
- **Footer is `title` (the model) on the left, effort on the right.** Both
  field names are unchanged from v3.3.0; only where they render moved. Cost
  used to occupy the footer's left slot; it duplicated a number Claude Code's
  own UI already shows, so that slot now carries the model instead.

### Removed
- **`footer.left` (cost).** The device no longer reads or draws it. A pusher
  that still sends it does no harm - the value is simply ignored, same as any
  other field this firmware does not recognise.

### Notes
- No new fields, no renamed fields. A pusher older than this release still
  renders correctly: `session` and `title` land in the same places they always
  did on the wire, only the SCREEN POSITION of `title` changed.

## [3.3.0] - 2026-08-13

The header returns, and the four identity fields each get their own corner.

```
clauled-pusher          Opus 5
────────────────────────────────
5h reset      4h25m         7%
███░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
ctx         357k/1M        45%
██████████████░░░░░░░░░░░░░░░░░░
/ Running Bash
────────────────────────────────
$0.11                    xhigh
```

### Added
- **A header row**: the session on the left, the model on the right.
- **`session`** — a new top-level string, rendered at the header's left. Where
  the model says *what* is answering, this says *which work* it is answering
  about, which matters as soon as more than one session is open.
- **`footer.right`** — the effort level, spelled out. It has the corner to
  itself now, so `xhigh` no longer has to be squeezed to `xhi`.

### Changed
- **`title` is the model alone.** It previously carried model and effort joined,
  which meant a long model name truncated the effort away entirely. They are
  separate fields in separate corners, so neither can crowd out the other.
- **Bars return to 6 pixels**, as they were before v3.1.0. This is the cost of
  the header coming back: v3.1.0 bought 11-pixel bars by removing it, and that
  trade is now reversed. Knowing which session the numbers belong to was judged
  worth more than bar legibility.

### Notes
- **The schema is unchanged — `v` is still 3.** Both new fields are optional
  additions, so a pusher older than v3.2.0 still renders correctly: its combined
  `"Opus 5 xhigh"` simply lands in the header's right, and the footer's right
  stays empty. Nothing breaks; the effort is just less prominent.
- `session_name` is the natural source for the header's left, but Claude Code
  sends it rarely — once in fifty payloads, in the captures behind this. The
  plugin falls back to the workspace directory name, which is nearly always
  present and arguably the more useful answer.

## [3.2.0] - 2026-08-13

### Changed
- **The two data rows are now three columns**: label flush left, detail
  centred, percentage flush right.

  ```
  5h reset      4h25m         7%
  ███░░░░░░░░░░░░░░░░░░░░░░░░░░░
  ctx         357k/1M        45%
  ██████████████░░░░░░░░░░░░░░░░
  ```

  Previously all three ran together as one left-aligned string. Separating them
  gives each a fixed place to look, which is the point of a glanceable display.

  The detail is centred on the **screen**, not on the gap between its
  neighbours, so it stays put as the percentage widens from `7%` to `100%` — a
  value that shifts every time the number beside it changes is harder to read at
  a glance than one that never moves.

  It moves only to avoid a collision, and then it nudges rather than overlaps:
  there is always at least one blank character on each side. With `5h reset` at
  48 px, true centring would leave a 1-pixel gap, so the quota row's detail sits
  5 px right of centre. The context row centres exactly. A guaranteed gap is
  worth more than pixel-perfect symmetry — 1 px reads as two words touching.

  If even the nudge will not fit, the detail is dropped and the percentage
  survives. That is the right way round: the percentage is what you came for.

### Notes
- No pusher change is required. `gauge.label`, the paired `row` side and
  `gauge.pct` are the same three fields as before; only their placement changed.
- The 21-character line budget still applies per column rather than to a
  composed string, so labels have more room than they did in v3.1.0.

## [3.1.0] - 2026-08-13

A denser screen. Dropping the top header and the USB indicator paid for bars
nearly twice as tall, and sleep no longer takes the display away from you.

### Changed
- **New layout.** Every one of the 64 rows is now budgeted:

  ```
   0- 7   text    5h reset 4h33m 55%
   9-19   bar     quota                    (11 px, was 6)
  21-28   text    ctx 357k/1M 45%
  30-40   bar     context                  (11 px, was 6)
  43-53   status  banner / spinner / sleep
  55      rule
  56-63   text    Opus 5 xhigh      $0.11
  ```

  Each gauge is one text line and one bar, and the model, effort and cost move
  to the bottom, where they never change shape and work as an anchor for the
  eye. A 6-pixel bar with a 4-pixel fill was hard to read from across a desk;
  this is the change that fixes that.
- **Gauge lines are composed on the device**, from the gauge label, its paired
  `row` detail and its percentage. The 21-character budget is now enforced
  where the pixels are rather than trusted to the host. If the line will not
  fit, the countdown is dropped before the percentage — 100% is one character
  wider than every other value, and it is exactly when the row most needs to
  be readable.
- **The schema is unchanged.** `v` is still 3 and every field means what it
  meant; only their positions moved. No pusher change is required, though
  v3.1.0 of the plugin sends shorter labels that suit the new rows.

### Removed
- **The `USB ok` / `USB idle` indicator.** It could never distinguish "Claude
  Code is closed" from "the cable is unplugged", and the status row already
  says whether anything is happening.
- **The top header row.** The model and effort it carried now live at the
  bottom next to the cost.

### Fixed
- **Sleep no longer takes over the whole screen.** It is confined to the status
  row, so the gauges stay readable while the host is away — which is the one
  time you are most likely to glance over casually, and previously the one time
  there was nothing to see. The cost is the animation: an 11-pixel row cannot
  hold the growing `Z`s of the old full-screen version, since a size-2 glyph is
  already 16 pixels tall. What remains is a horizontal march with a slight
  rise, alongside the sleeping face and how long it has been quiet.

### Documentation
- **The README claimed v3.0.0 as the latest release** through the whole of
  v3.0.1, and `API.md`'s status example reported the same stale number. The
  release process is supposed to move `version.h`, `CHANGELOG.md` and
  `README.md` together; it did not.
- **The `flashing-firmware` skill still described the pre-v2.0.0 architecture.**
  It asked for WiFi credentials and a push key, told you to create
  `src/secrets.h`, and verified the flash by curling `clauled.local/health` —
  none of which has existed since v2.0.0. Anyone following it would have gone
  looking for a file deleted two releases ago. It now covers the real process,
  and requires checking the reported `version=` against `version.h`: a device
  can otherwise run a fix while reporting an older number, which is exactly
  what happened between v3.0.0 and v3.0.1 and left the version string
  untrustworthy for diagnosis.

### Notes
- Both data rows are tight. `5h reset 4h33m 100%` is 19 characters and
  `ctx 357k/1M 100%` is 16, against a 21-character line. The plugin's selftest
  asserts both at 100% so a label change cannot quietly overflow them.

## [3.0.1] - 2026-08-13

### Fixed
- **A banner masked the spinner for its full lifetime.** Events cleared `busy`,
  but `busy` did not clear events, so a `Your turn` banner held the detail row
  for its entire 5-minute TTL while every spinner push landed invisibly beneath
  it. The display looked frozen mid-task even though hooks were firing normally.
  Going busy now clears any banner: if Claude is working, it is not your turn.

## [3.0.0] - 2026-08-13

Everything on one screen: two gauges, a live activity line, and a sleep
animation. The protocol moves to labelled display fields, so the host decides
what each gauge means and new data sources need no firmware change.

### Changed — breaking
- **Protocol carries labelled fields, not fixed metrics.** `gauge1`, `gauge2`,
  `row`, `footer`, `title` and `busy` replace the `usage.five_hour` /
  `seven_day` buckets. Schema version is now `3`.
- **Page cycling is gone.** Everything renders on a single screen, so
  `CYCLE_TIME`, `SHOW_WEEKLY_SONNET` and `SHOW_UPTIME` have been removed from
  `config.h`.

### Added
- **Two gauges.** The host labels them; the plugin uses them for the 5h
  subscription quota and context-window occupancy.
- **Activity line.** The detail row shows what Claude is doing —
  `/ Running Bash`, `- Editing main.cpp`, or a gerund while it thinks. The
  spinner animates on device time, so a long turn keeps moving with no pushes.
- **Sleep animation.** After `STALE_AFTER_S` with no push the device decides the
  host is asleep or Claude Code is closed and switches to a `(-_-)` face with
  Z's drifting up and away. It cannot tell which, and does not pretend to.
- **Attention banners** are inverted — white block, black text — deliberately
  the loudest thing on the screen.
- The BOOT button clears a stuck banner or spinner immediately.

### Fixed
- The busy state self-expires after 3 minutes, so a missed `Stop` hook cannot
  leave the spinner running forever.

### Notes
- Detail row priority is banner → spinner → static detail, so an alert always
  wins over activity.
- A gauge with `pct` below zero renders `--` with an empty bar. One feed being
  unavailable never blanks the other.

## [2.0.0] - 2026-08-13

The device moves to USB serial and drops networking entirely. It now holds no
credentials of any kind — not a Claude token, not a WiFi password, not a shared
key. The payload schema is unchanged; only the transport differs.

### Changed — breaking
- **Transport is USB serial, not HTTP.** The device reads newline-delimited
  JSON from the serial port and answers each line with a single-line JSON
  acknowledgement. `POST /push` and `GET /health` are gone.
  `{"v":1,"cmd":"status"}` replaces the health endpoint.
- **`secrets.h` is gone.** Configuration moved to `src/config.h`, which is
  tracked in git because nothing in it is secret. There is no copy step and
  nothing to fill in before flashing.
- The payload schema is unchanged — the same `v`, `usage`, `resets_in` and
  `events` fields, and the same merge semantics — so a pusher only has to
  change how it sends, not what.

### Removed
- The entire WiFi stack: station mode, reconnection handling, mDNS, the web
  server, and every diagnostic built for them (I2C bus scanning is retained;
  WiFi scanning, disconnect-reason decoding, MAC override and PMF/auth-mode
  reporting are not).
- WiFi credentials and the shared push key. The device holds no credentials, so
  release binaries no longer contain anything sensitive.
- Endpoint authentication, which physical USB access replaces.

### Added
- `{"v":1,"cmd":"status"}` probe returning firmware version, display state,
  uptime and last-push age, so a host can confirm a port really is a Clauled
  device before pushing to it.
- Human-readable log output is prefixed with `# ` so it is distinguishable from
  protocol replies, which always begin with `{`.
- The footer shows USB link state (`USB ok` / `USB idle`) in place of the IP.

### Impact
- Flash usage dropped from 64.4% to **22.7%** (844,594 -> 297,606 bytes)
- RAM usage dropped from 12.7% to **4.4%** (41,628 -> 14,412 bytes)

## [1.1.0] - 2026-08-13

Display and network diagnostics. The device now drives SPI panels as well as
I2C ones, and neither a missing display nor an unreachable network can take it
down.

### Added
- **SPI display support** for 7-pin modules (`GND VCC D0/SCK D1/SDA RES DC CS`),
  selected with `DISPLAY_SPI` in `secrets.h`. Uses software SPI, so any free
  GPIOs work and no peripheral setup is needed. I2C modules are unaffected.
- **Configurable display pins** in `secrets.h` — `SDA_PIN`/`SCL_PIN` for I2C and
  `OLED_MOSI`/`OLED_CLK`/`OLED_DC`/`OLED_RST`/`OLED_CS` for SPI, with the
  ESP32-C3 safe-pin list documented alongside.
- **I2C bus scanner**, run automatically when no display is found. Sweeps all
  126 addresses in both pin orders and reports a verdict that separates
  "nothing on the bus" from "SDA and SCL reversed" from "present at another
  address".
- **WiFi diagnostics**: visible networks with RSSI, channel and security mode;
  the target AP's BSSID and auth type; and disconnect reason codes translated
  into plain language such as `wrong password`, `AP client limit`,
  `AP not found` and `AP kicked us`.
- **Station config dump** — SSID, password *length* (never the value), PMF
  capability, minimum auth mode, and the device MAC.
- **`WIFI_MAC_OVERRIDE`** diagnostic, for testing whether a router filters by
  MAC address.
- `CORE_DEBUG_LEVEL` documented in `platformio.ini` for exposing the ESP-IDF
  WiFi state machine over USB serial.

### Changed
- **The display no longer depends on the network.** Boot tries WiFi twice then
  carries on, retrying every 30 seconds in the background without ever stalling
  the render loop. Previously a router that refused the device left it stuck on
  "Connecting..." forever and it never reached the display code at all.
- WiFi retries no longer power-cycle the radio between attempts. `disconnect(true)`
  switches WiFi off entirely, which cold-started the stack on every attempt and
  made the AP see a client flapping on and off.
- The driver's auto-reconnect is disabled so there is one retry policy rather
  than two racing each other; this cut retry storms from roughly 25 events per
  30 seconds to 2.
- Connect timeout raised from 15 to 20 seconds to allow for slower DHCP, and
  modem sleep disabled.

### Fixed
- The WiFi scan diagnostic reported "SSID not visible" when the scan had in fact
  **failed**: `WIFI_SCAN_FAILED` (-2) was being treated as a network count. A
  scan cannot run while an association attempt is in flight, so the attempt is
  now stopped first, and a failed scan reports "scan failed" rather than
  asserting anything about visibility. The old behaviour produced actively
  misleading diagnoses.
- `releasing-firmware` skill: stages all outstanding work rather than two files,
  requires a bash heredoc for commit messages, documents that amending a commit
  orphans its tag, and requires `--repo` on `gh release create` — on a fork,
  `gh` otherwise targets the upstream parent repository.

## [1.0.0] - 2026-08-12

First version of the push-based architecture. Forked from
[rafbanaan/clauled](https://github.com/rafbanaan/clauled) and reworked so the
device holds no Claude credentials.

### Added
- `POST /push` ingest endpoint, authenticated with a shared `X-Clauled-Key`
  header compared in constant time.
- `GET /health` for reachability checks — exposes uptime, version, and last-push
  age, and nothing sensitive.
- mDNS advertisement, so the device is reachable at `clauled.local`.
- Compile-time configuration in a gitignored `src/secrets.h`, with a tracked
  `src/secrets.h.example` template.
- `static_assert` guards that turn missing configuration into a build error
  with an actionable message.
- Event pages, with word wrapping and a TTL, ready for notification pushes.
- Staleness tracking in the footer, shown as information rather than an error.
- Graceful degradation when the OLED is absent: the device logs the failure,
  reports `display_ok: false` from `/health`, and carries on serving pushes.
  Previously a missing display halted boot in an infinite loop, taking WiFi and
  the endpoint down with it.
- `API.md` documenting the push contract.

### Removed
- The Claude OAuth token and everything that used it — the device no longer
  contacts `api.anthropic.com` or `console.anthropic.com` at all.
- The OAuth refresh flow.
- The first-boot WiFi setup portal, which ran an **open** access point serving
  a plaintext HTTP form that you typed your token into.
- The unauthenticated browser config page and all of its write endpoints,
  along with `data/index.html` and its screenshots.
- LittleFS and the `uploadfs` flashing step — no filesystem image is needed.
- NTP and all wall-clock handling. Reset countdowns now arrive as seconds
  remaining and tick locally.

### Fixed
- README referenced a `Wire.setClock(400000)` call that did not exist in the
  source.
