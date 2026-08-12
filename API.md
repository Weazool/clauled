# Clauled push API

The device is a display, not an API client. It holds no Claude credentials and never
contacts Anthropic. Something on your desktop fetches the numbers and pushes them here.

Base URL: `http://clauled.local` (mDNS), or `http://<device-ip>` if mDNS is unavailable.

## `POST /push`

Headers:

| Header | Required | Value |
|---|---|---|
| `Content-Type` | yes | `application/json` |
| `X-Clauled-Key` | yes | must equal `CLAULED_PUSH_KEY` from `src/secrets.h` |

Body:

```json
{
  "v": 1,
  "usage": {
    "five_hour":        { "pct": 23.5, "resets_in": 4920 },
    "seven_day":        { "pct": 41.2, "resets_in": 340000 },
    "seven_day_sonnet": { "pct": 12.0, "resets_in": 340000 }
  },
  "events": [
    { "type": "attention", "text": "Claude needs input" }
  ]
}
```

| Field | Type | Notes |
|---|---|---|
| `v` | int | Schema version. Currently `1`. A mismatch is rejected rather than guessed at. |
| `usage.*.pct` | float | 0–100, percent **used**. |
| `usage.*.resets_in` | int | **Seconds remaining**, not an absolute timestamp. |
| `events[].type` | string | Short label, shown as the page title. Truncated to 21 chars. |
| `events[].text` | string | Body text, wrapped over 2 lines. Truncated to 40 chars. |

Responses:

| Code | Meaning |
|---|---|
| `200` | `{"ok":true}` |
| `400` | Malformed JSON, missing body, or unsupported `v` |
| `401` | Missing or incorrect `X-Clauled-Key` |
| `413` | Body over 2 KB |

### Semantics that matter

**Everything is optional and pushes merge.** A push containing only `events` leaves the
usage bars intact, and vice versa. Send partial payloads freely.

**`resets_in` is seconds, deliberately.** The device has no wall clock — no NTP, no
timezone handling. It stamps the arrival time and counts down locally, so the display
stays accurate between pushes. Your pusher does the `resets_at - now` subtraction.

**Staleness is not an error.** The device tracks time since the last accepted push and
shows it in the footer, marking it stale past `STALE_AFTER_S` (default 300s). Since the
pusher only runs while Claude Code is open, an overnight gap is expected and reads as
information rather than a fault.

**Events expire.** The most recent event is shown on its own page for `EVENT_TTL_S`
(default 300s), then the page disappears. Only the last event in an array is retained;
at most 8 are parsed per request.

## `GET /health`

Unauthenticated. Exposes nothing sensitive — no usage figures, no SSID — so the pusher
can check reachability before building a payload.

```json
{ "ok": true, "version": "1.0.0", "display_ok": true, "uptime": 3600, "last_push_age": 42, "schema": 1 }
```

| Field | Meaning |
|---|---|
| `version` | Firmware version, so you can tell what is actually flashed |
| `display_ok` | Whether the OLED was detected at boot. `false` means the device is running **headless** — it still accepts pushes, it just cannot show them |
| `last_push_age` | Seconds since the last accepted push, or `-1` if there has never been one |

A missing display is deliberately not fatal. If the I2C wiring fails, the device
still joins WiFi and serves this endpoint, so the fault stays diagnosable over
the network instead of requiring a USB cable.

## Example

```bash
curl -X POST http://clauled.local/push \
  -H "X-Clauled-Key: $CLAULED_PUSH_KEY" \
  -H "Content-Type: application/json" \
  -d '{"v":1,"usage":{"five_hour":{"pct":23.5,"resets_in":4920}}}'
```

## Notes for the pusher

The intended source is a Claude Code statusline script, which receives a `rate_limits`
object on stdin containing the 5h and 7d percentages — no token required anywhere in the
system. Hooks do **not** receive rate-limit data, so events (`Stop`, `Notification`) and
usage come from different mechanisms.

`resets_at` in that payload is an absolute epoch; convert to seconds-remaining before
pushing.
