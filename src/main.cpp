// ============================================================
//  Clauled - Claude Code status display for ESP32-C3 + SH1106 OLED
//
//  The device has no network stack and holds no credentials. It reads
//  newline-delimited JSON from USB serial and renders it on one screen.
//
//  The protocol carries labelled display fields rather than fixed metrics:
//  the host decides what each gauge means, so new data sources need no
//  firmware change. See API.md.
//
//  Configuration is in src/config.h (tracked - nothing secret).
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#include "config.h"
#include "version.h"

// ── Pins ──────────────────────────────────────────────────────
#define BOOT_BTN  9

// ── Display ───────────────────────────────────────────────────
#define SCREEN_W   128
#define SCREEN_H   64
#define CHAR_W     6      // default GFX font advance at size 1
#define LINE_CHARS 21     // 128 / 6

// ── Layout ────────────────────────────────────────────────────
// All 64 rows are budgeted.
//
//    0- 7   header  session name          N/M
//    9      rule
//   11-18   text    "5h"   "4h25m"     "7%"     <- alternates with "1w" every
//   20-25   bar     account quota                  ROTATE_INTERVAL_S, on its
//                                                   own clock - independent of
//                                                   which session is shown
//   27-34   text    "ctx"      "357k/1M"    "45%"
//   36-41   bar     context (per session)        (6 px)
//   43-53   status  banner / spinner / sleep
//   55      rule
//   56-63   footer  model (always, see below)  effort
#define HEADER_Y       0
#define HEAD_RULE_Y    9
#define ROW1_Y        11
#define BAR1_Y        20
#define ROW2_Y        27
#define BAR2_Y        36
#define BAR_H          6
#define STATUS_TEXT_Y 45     // 7-px glyph centred in the status row
#define RULE_Y        55
#define BOTTOM_Y      56

// ── Protocol limits ───────────────────────────────────────────
// A malformed or hostile line must not be able to exhaust heap.
#define SCHEMA_VERSION   3
#define LINE_MAX         2048
#define FIELD_MAX        21     // one screen line
#define EVENT_TTL_S      300
#define BUSY_TTL_S       180    // a missed Stop hook must not spin forever
#define MAX_SESSIONS       8    // bounded roster - a hard cap, not a tuning knob

#ifdef DISPLAY_SPI
// Software SPI: works on any GPIOs with no peripheral setup, and the 1 KB
// framebuffer at ~3 Hz makes bitbang speed a non-issue.
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, OLED_MOSI, OLED_CLK, OLED_DC, OLED_RST, OLED_CS);
#else
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);
#endif

// ── Pushed state ──────────────────────────────────────────────
//
// Two different kinds of state, kept deliberately separate:
//
//   - The account quota (row 1) and the last known model (footer left) are
//     the SAME for every session under this login - they are not really
//     "session data" at all - so they live as plain globals, outside the
//     roster, and survive it going empty.
//   - Everything else (context, effort, activity, name) is genuinely
//     per-session and lives in a Session struct, one per concurrent
//     Claude Code session pushing to this device.

// Account-level quota. "5h" and "1w" are the host's labels, not hardcoded
// here - only the SLOT (five-hour vs weekly) is fixed, same as gauge1/gauge2
// were always host-labelled.
String        q5Label = "", q5Reset = "";
float         q5Pct   = -1;
String        qwLabel = "", qwReset = "";
float         qwPct   = -1;
bool          quotaShowWeek = false;
unsigned long quotaRotateMs = 0;

// Last known model, independent of any one session - see drawScreen() for
// why the footer falls back to this rather than ever going blank.
String lastModel = "";

struct Session {
  bool   used = false;
  String sid  = "";

  String title  = "";       // this session's own last-known model
  String g2Label = "";
  float  g2Pct   = -1;
  String rowR    = "";
  String effort  = "";
  String name    = "";      // header's left

  String        busyText = "";
  unsigned long busyAt   = 0;
  String        eventText = "";
  unsigned long eventAt   = 0;

  unsigned long lastPushMs = 0;
};

Session sessions[MAX_SESSIONS];

bool          quietHours    = false;  // host-computed: is it currently the quiet window?
unsigned long lastAnyPushMs = 0;      // idle clock for quiet-hours - ANY session counts
bool          everAnyPushed = false;

int           lastShownSlot = -1;     // which array index is on screen right now
unsigned long lastRotateMs  = 0;

// ── Runtime ───────────────────────────────────────────────────
unsigned long lastDraw  = 0;
bool          displayOk = false;
bool          displayAsleep  = false;  // panel powered off for quiet-hours idle
bool          displayInverted = false; // whole panel inverted - a banner is live
String        lineBuf;
bool          lineOverflow = false;

// ── Helpers ───────────────────────────────────────────────────
String fmtAge(unsigned long ms) {
  unsigned long s = ms / 1000;
  if (s < 60)   return String(s) + "s";
  if (s < 3600) return String(s / 60) + "m";
  return String(s / 3600) + "h";
}

bool slotEventLive(const Session& s) {
  if (s.eventText.length() == 0) return false;
  return ((millis() - s.eventAt) / 1000UL) <= (unsigned long)EVENT_TTL_S;
}

bool slotBusyLive(const Session& s) {
  if (s.busyText.length() == 0) return false;
  return ((millis() - s.busyAt) / 1000UL) <= (unsigned long)BUSY_TTL_S;
}

// "Quiet a while" for THIS session specifically - shows its sleep row when it
// is the one on screen. Distinct from shouldPowerDown() below, which is about
// every session at once; one session going quiet must not blank the panel
// while a different one is still actively being used.
bool slotStale(const Session& s) {
  return ((millis() - s.lastPushMs) / 1000UL) > (unsigned long)STALE_AFTER_S;
}

/**
 * Should the panel be dark right now?
 *
 * Two conditions, from two different clocks: quietHours comes from the host,
 * which has a real one; the idle duration comes from millis(), which this
 * device has always had. quietHours can only ever be as fresh as the last
 * push - if the host goes silent for hours (Claude Code fully closed) spanning
 * a quiet-hours boundary, this reads whatever the last real push said. That
 * mirrors every other host-computed value here (the 5h countdown has the same
 * limitation) and is the accepted cost of having no network stack and no NTP.
 *
 * Uses lastAnyPushMs - every session's idle clock at once - deliberately not
 * whichever session happens to be on screen. One quiet session must not power
 * off a panel that a DIFFERENT session is actively using.
 */
bool shouldPowerDown() {
  if (!quietHours) return false;
  return ((millis() - lastAnyPushMs) / 1000UL) > (unsigned long)QUIET_IDLE_S;
}

// Human-readable logging goes out with a '#' prefix so the host can tell it
// apart from protocol replies, which are always a JSON object on one line.
void logf(const char* fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print("# ");
  Serial.println(buf);
}

// ── Session roster ────────────────────────────────────────────

/** A session with no push in SESSION_GONE_S is assumed closed - dropped. */
void pruneSessions() {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].used && ((millis() - sessions[i].lastPushMs) / 1000UL) > (unsigned long)SESSION_GONE_S) {
      sessions[i] = Session();
    }
  }
}

/** Indices of every occupied slot, in array order - stable as slots free up. */
int listUsed(int* out) {
  int n = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used) out[n++] = i;
  return n;
}

int findOrCreateSlot(const String& sid) {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].used && sessions[i].sid == sid) return i;
  }
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].used) {
      sessions[i] = Session();
      sessions[i].sid  = sid;
      sessions[i].used = true;
      return i;
    }
  }
  // Full - eight concurrent sessions is already a lot. Evict whichever has
  // gone longest without a push rather than refuse the newest one.
  int oldest = 0;
  for (int i = 1; i < MAX_SESSIONS; i++) {
    if (sessions[i].lastPushMs < sessions[oldest].lastPushMs) oldest = i;
  }
  sessions[oldest] = Session();
  sessions[oldest].sid  = sid;
  sessions[oldest].used = true;
  return oldest;
}

/**
 * Which slots belong to the group that should be shown right now, and how
 * many. Attention beats working beats idle - the same "alert always wins"
 * rule a single session already had, now deciding which SESSIONS get shown,
 * not only what one session's status row displays.
 */
int groupMembers(int* out) {
  int used[MAX_SESSIONS];
  int n = listUsed(used);

  int m = 0;
  for (int i = 0; i < n; i++) if (slotEventLive(sessions[used[i]])) out[m++] = used[i];
  if (m) return m;

  for (int i = 0; i < n; i++) if (slotBusyLive(sessions[used[i]])) out[m++] = used[i];
  if (m) return m;

  for (int i = 0; i < n; i++) out[m++] = used[i];   // idle - everyone left
  return m;
}

/**
 * Pick which slot to show, advancing rotation on its own 3-second clock.
 *
 * If the currently-shown slot is no longer in the active group - it was
 * promoted into attention, demoted out of it, or pruned entirely - snap to
 * the front of the new group immediately rather than waiting for the next
 * tick. Otherwise advance one step every ROTATE_INTERVAL_S, wrapping around.
 *
 * Entirely independent of the quota row's own 5h/1w alternation below - two
 * separate rotations, two separate clocks, because they rotate two
 * unrelated things (which session; which account metric).
 */
int pickSlotToShow() {
  int group[MAX_SESSIONS];
  int m = groupMembers(group);
  if (m == 0) return -1;

  int pos = -1;
  for (int i = 0; i < m; i++) if (group[i] == lastShownSlot) { pos = i; break; }

  if (pos < 0) {
    lastShownSlot = group[0];
    lastRotateMs  = millis();
    return lastShownSlot;
  }

  if (m > 1 && millis() - lastRotateMs >= (unsigned long)ROTATE_INTERVAL_S * 1000UL) {
    pos = (pos + 1) % m;
    lastRotateMs = millis();
  }
  lastShownSlot = group[pos];
  return lastShownSlot;
}

/** "2/5" - position of shownIdx among ALL active sessions, not just its group. */
String positionLabel(int shownIdx) {
  int used[MAX_SESSIONS];
  int m = listUsed(used);
  int n = 0;
  for (int i = 0; i < m; i++) { n++; if (used[i] == shownIdx) break; }
  return String(n) + "/" + String(m);
}

// ── Rendering ─────────────────────────────────────────────────
void drawRight(int y, const String& s) {
  if (s.length() == 0) return;
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_W - (int)w, y);
  display.print(s);
}

void drawLR(int y, const String& l, const String& r) {
  if (l.length()) { display.setCursor(0, y); display.print(l.substring(0, LINE_CHARS)); }
  drawRight(y, r);
}

void drawCenter(int y, const String& text) {
  if (!text.length()) return;
  String t = text.substring(0, LINE_CHARS);
  int px = (SCREEN_W - (int)t.length() * CHAR_W) / 2;
  display.setCursor(px < 0 ? 0 : px, y);
  display.print(t);
}

String pctText(float pct) {
  return (pct >= 0) ? String((int)(pct + 0.5f)) + "%" : "--";
}

/**
 * One data row in three columns: label left, detail centred, percentage right.
 *
 * The detail is centred on the SCREEN, not on the gap between its neighbours,
 * so it stays put as the percentage widens from "7%" to "100%". A value that
 * shifts every time the number beside it changes is harder to read at a glance
 * than one that never moves.
 *
 * It only moves to avoid a collision, and then it nudges rather than overlaps -
 * there is always at least one blank character on each side. If even that will
 * not fit, the detail is dropped: the percentage is the thing you came for.
 */
void drawRow3(int y, const String& left, const String& mid, const String& right) {
  String l = left.substring(0, LINE_CHARS);
  String m = mid.substring(0, LINE_CHARS);
  String r = right.substring(0, LINE_CHARS);

  const int lw = (int)l.length() * CHAR_W;
  const int mw = (int)m.length() * CHAR_W;
  const int rw = (int)r.length() * CHAR_W;

  if (l.length()) { display.setCursor(0, y); display.print(l); }
  if (r.length()) { display.setCursor(SCREEN_W - rw, y); display.print(r); }
  if (!m.length()) return;

  const int minX = lw ? lw + CHAR_W : 0;
  const int maxX = SCREEN_W - rw - (rw ? CHAR_W : 0) - mw;
  if (maxX < minX) return;                 // no room between them; drop it

  int x = (SCREEN_W - mw) / 2;
  if (x < minX) x = minX;
  if (x > maxX) x = maxX;

  display.setCursor(x, y);
  display.print(m);
}

void drawBar(int y, float pct) {
  display.drawRect(0, y, SCREEN_W, BAR_H, SH110X_WHITE);
  if (pct >= 0) {
    int fill = (int)((SCREEN_W - 2) * constrain(pct, 0.0f, 100.0f) / 100.0f);
    if (fill > 0) display.fillRect(1, y + 1, fill, BAR_H - 2, SH110X_WHITE);
  }
}

/**
 * Row 1: the account quota, alternating 5h and weekly every ROTATE_INTERVAL_S.
 *
 * Deliberately global, not tied to any session - the quota is the same
 * number for everyone under this login, so it does not belong to whichever
 * session happens to be on screen. That is also why it keeps working once
 * the roster is empty: see the m==0 branch in drawScreen().
 *
 * Only alternates if a weekly reading has actually arrived - an older host
 * that never sends gauge3 just leaves the 5h row showing permanently, the
 * same "absence degrades gracefully" rule every other field here follows.
 */
void drawQuotaRow() {
  bool hasWeek = qwPct >= 0;
  if (hasWeek && millis() - quotaRotateMs >= (unsigned long)ROTATE_INTERVAL_S * 1000UL) {
    quotaShowWeek = !quotaShowWeek;
    quotaRotateMs = millis();
  }
  bool showWeek = hasWeek && quotaShowWeek;
  if (showWeek) {
    drawRow3(ROW1_Y, qwLabel, qwReset, pctText(qwPct));
    drawBar(BAR1_Y, qwPct);
  } else {
    drawRow3(ROW1_Y, q5Label, q5Reset, pctText(q5Pct));
    drawBar(BAR1_Y, q5Pct);
  }
}

// Spinner turns on the device, so a long turn keeps moving with no pushes.
void drawBusy(const String& text) {
  static const char frames[] = {'|', '/', '-', '\\'};
  char c = frames[(millis() / 300) % 4];
  display.setCursor(0, STATUS_TEXT_Y);
  display.print(c);
  display.setCursor(2 * CHAR_W, STATUS_TEXT_Y);
  display.print(text.substring(0, LINE_CHARS - 2));
}

/**
 * Sleep, confined to the status row so the gauges stay readable.
 *
 * sinceMs is the SLOT's own lastPushMs, not a global - each session in the
 * roster reports its own quiet duration when it is the one on screen.
 */
void drawSleepRow(unsigned long sinceMs) {
  display.setCursor(0, STATUS_TEXT_Y);
  display.print("(-_-)");
  display.setCursor(6 * CHAR_W, STATUS_TEXT_Y);
  display.print(fmtAge(millis() - sinceMs));

  const unsigned long f = millis() / 300;
  for (int i = 0; i < 3; i++) {
    int phase = (int)((f + i * 3) % 9);
    if (phase > 5) continue;                 // a gap, so they don't crowd
    int zx = 66 + phase * 10;
    if (zx > SCREEN_W - CHAR_W) continue;
    display.setCursor(zx, STATUS_TEXT_Y - phase / 2);
    display.print('z');
  }
}

/**
 * The bigger idle graphic for the m==0 screen - every session has aged out,
 * so there is a whole lower half of the screen with nothing session-specific
 * left to draw in it (context occupancy does not mean anything without a
 * session). Reuses the pre-v3.1.0 growing-Z animation, which had exactly
 * this much room before the header/footer redesigns took the rest of the
 * screen for gauges - it fits again now that this is the only content below
 * the quota row.
 */
void drawIdleGraphic() {
  drawCenter(38, "(-_-)");
  const unsigned long f = millis() / 300;
  for (int i = 0; i < 3; i++) {
    int phase = (int)((f + i * 3) % 9);
    if (phase > 5) continue;
    display.setTextSize(1 + phase / 2);
    int zx = 44 + phase * 10;
    if (zx <= SCREEN_W - CHAR_W) {
      display.setCursor(zx, 56 - phase * 6);
      display.print('Z');
    }
  }
  display.setTextSize(1);
}

/**
 * force=true bypasses the power-down gate for exactly one call - used by the
 * BOOT button so it is not completely dead at 3am. It does not stay awake:
 * the very next loop() tick re-applies shouldPowerDown() and, since pressing
 * a button does not touch lastAnyPushMs, puts it straight back to sleep. That
 * gives a brief flash to confirm the device is alive, not a sustained peek.
 */
void drawScreen(bool force = false) {
  if (!displayOk) return;

  if (!force && shouldPowerDown()) {
    if (!displayAsleep) {
      display.oled_command(SH110X_DISPLAYOFF);
      displayAsleep = true;
    }
    return;
  }
  if (displayAsleep) {
    display.oled_command(SH110X_DISPLAYON);
    displayAsleep = false;
  }

  pruneSessions();

  int usedList[MAX_SESSIONS];
  int m = listUsed(usedList);

  if (m == 0) {
    const bool inverted = false;
    if (inverted != displayInverted) { display.invertDisplay(inverted); displayInverted = inverted; }

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);

    if (!everAnyPushed) {
      // Never seen ANY data at all, this boot - nothing to show, not even a
      // quota reading.
      drawCenter(20, "Waiting for data");
      drawCenter(32, "over USB serial");
      display.drawLine(0, RULE_Y, SCREEN_W - 1, RULE_Y, SH110X_WHITE);
      drawLR(BOTTOM_Y, "Clauled", "v" CLAULED_VERSION);
    } else {
      // Every session that WAS active has aged out (SESSION_GONE_S). The
      // account quota and the last known model both survive that - they were
      // never session data to begin with - so this is not a blank screen,
      // just an honest "nobody's here right now."
      drawCenter(HEADER_Y, "Idle");
      display.drawLine(0, HEAD_RULE_Y, SCREEN_W - 1, HEAD_RULE_Y, SH110X_WHITE);
      drawQuotaRow();
      drawIdleGraphic();
      display.drawLine(0, RULE_Y, SCREEN_W - 1, RULE_Y, SH110X_WHITE);
      drawLR(BOTTOM_Y, lastModel, "");
    }
    display.display();
    return;
  }

  int shownIdx = pickSlotToShow();
  Session& s = sessions[shownIdx];

  // A live banner on the session BEING SHOWN inverts the WHOLE panel, not
  // just its own row - the loudest possible signal that it is your move.
  // invertDisplay() is a single controller command (SH110X_INVERTDISPLAY)
  // that flips every pixel's polarity in hardware; it does not touch the
  // framebuffer, so everything drawn below comes out inverted for free.
  // Toggled only on change, not resent every redraw.
  const bool inverted = slotEventLive(s);
  if (inverted != displayInverted) {
    display.invertDisplay(inverted);
    displayInverted = inverted;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  // Header: session name left, position among ALL active sessions right -
  // "2/5" even while only cycling the subset that needs attention, so the
  // number tells you the true scale, not just how big today's rotation is.
  drawLR(HEADER_Y, s.name, positionLabel(shownIdx));
  display.drawLine(0, HEAD_RULE_Y, SCREEN_W - 1, HEAD_RULE_Y, SH110X_WHITE);

  drawQuotaRow();   // account-level - same content regardless of which session this is

  drawRow3(ROW2_Y, s.g2Label, s.rowR, pctText(s.g2Pct));
  drawBar(BAR2_Y, s.g2Pct);

  // Status row resolves in priority order: an alert always wins over a
  // spinner, a spinner over sleep. Nothing at all is a legitimate state - it
  // means a turn ended more than EVENT_TTL_S ago but the host is still alive.
  //
  // The banner text draws NORMALLY here (plain white) - the whole screen is
  // already hardware-inverted above when a banner is live, so inverting this
  // one row again would cancel out and leave the text invisible.
  if      (slotEventLive(s)) drawCenter(STATUS_TEXT_Y, s.eventText);
  else if (slotBusyLive(s))  drawBusy(s.busyText);
  else if (slotStale(s))     drawSleepRow(s.lastPushMs);

  display.drawLine(0, RULE_Y, SCREEN_W - 1, RULE_Y, SH110X_WHITE);

  // Model left, effort right. The model falls back to the last one seen from
  // ANY session when this particular session has not reported one yet - hook
  // payloads never carry the model, so a session whose first-ever push is a
  // hook would otherwise show a blank left corner until its own statusline
  // eventually fires. A slightly-stale model name reads better than a blank.
  drawLR(BOTTOM_Y, s.title.length() ? s.title : lastModel, s.effort);

  display.display();
}

// ── Protocol ──────────────────────────────────────────────────
void reply(const char* json) { Serial.println(json); }

String field(JsonVariantConst v, const String& fallback) {
  if (v.isNull()) return fallback;
  return String((const char*)(v | "")).substring(0, FIELD_MAX);
}

void applyGauge(JsonVariantConst g, String& label, float& pct) {
  if (g.isNull()) return;
  if (!g["label"].isNull()) label = field(g["label"], label);
  if (!g["pct"].isNull())   pct   = g["pct"].as<float>();
}

void sendStatus() {
  int usedList[MAX_SESSIONS];
  JsonDocument doc;
  doc["ok"]            = true;
  doc["version"]       = CLAULED_VERSION;
  doc["display_ok"]    = displayOk;
  doc["uptime"]        = millis() / 1000;
  doc["last_push_age"] = everAnyPushed ? (long)((millis() - lastAnyPushMs) / 1000UL) : -1;
  doc["quiet_sleep"]   = displayAsleep;
  doc["sessions"]      = listUsed(usedList);
  doc["schema"]        = SCHEMA_VERSION;
  String out;
  serializeJson(doc, out);
  Serial.println(out);
}

void handleLine(const String& line) {
  if (line.length() == 0) return;

  JsonDocument doc;
  if (deserializeJson(doc, line)) { reply("{\"error\":\"bad JSON\"}"); return; }

  JsonObjectConst root = doc.as<JsonObjectConst>();

  int v = root["v"] | SCHEMA_VERSION;
  if (v != SCHEMA_VERSION) { reply("{\"error\":\"unsupported schema version\"}"); return; }

  const char* cmd = root["cmd"] | "";
  if (strcmp(cmd, "status") == 0) { sendStatus(); return; }

  // quiet is global, not per-session - a property of wall-clock time, the
  // same regardless of which session's push happened to carry it. Unlike
  // everything below, an ABSENT key leaves it unchanged, but the host sends
  // it on every push (true or false) so a stale "true" cannot outlive
  // quiet hours.
  if (!root["quiet"].isNull()) quietHours = root["quiet"].as<bool>();

  // gauge1/row.left (the 5h account quota) and gauge3 (the weekly one) are
  // ALSO global, for the same reason quiet is - the same number regardless
  // of which session reports it. Every push updates them, whichever session
  // it came from.
  JsonVariantConst g1 = root["gauge1"];
  if (!g1.isNull()) {
    if (!g1["label"].isNull()) q5Label = field(g1["label"], q5Label);
    if (!g1["pct"].isNull())   q5Pct   = g1["pct"].as<float>();
  }
  JsonVariantConst g3 = root["gauge3"];
  if (!g3.isNull()) {
    if (!g3["label"].isNull()) qwLabel = field(g3["label"], qwLabel);
    if (!g3["pct"].isNull())   qwPct   = g3["pct"].as<float>();
    if (!g3["reset"].isNull()) qwReset = field(g3["reset"], qwReset);
  }

  // No sid at all is not an error - it is the pre-multi-session contract,
  // and it still works: everything with no sid shares the one "" slot, which
  // is exactly the old single-session behaviour, M pinned at 1.
  String sid = field(root["sid"], "");
  int idx = findOrCreateSlot(sid);
  Session& s = sessions[idx];

  // Merge, never replace: a hook pushing only "busy" must not wipe this
  // session's gauges, and its statusline pushing only gauges must not clear
  // its busy state.
  if (!root["title"].isNull()) { s.title = field(root["title"], s.title); lastModel = s.title; }
  if (!root["session"].isNull()) s.name  = field(root["session"], s.name);
  applyGauge(root["gauge2"], s.g2Label, s.g2Pct);

  JsonVariantConst r = root["row"];
  if (!r.isNull()) {
    if (!r["left"].isNull()) q5Reset = field(r["left"], q5Reset);
    s.rowR = field(r["right"], s.rowR);
  }

  // footer.left is not read - it used to carry cost, which is gone. A pusher
  // still sending it does no harm; the value is simply never drawn.
  JsonVariantConst f = root["footer"];
  if (!f.isNull() && !f["right"].isNull()) s.effort = field(f["right"], s.effort);

  // An empty string clears this session's spinner - that is how Stop ends a turn.
  if (!root["busy"].isNull()) {
    s.busyText = field(root["busy"], "");
    s.busyAt   = millis();
    // Going busy also clears this session's own banner: if it is working, it
    // is not your turn on THAT one. Without this a "Your turn" banner
    // outranks every spinner beneath it for its full TTL.
    if (s.busyText.length()) s.eventText = "";
  }

  JsonArrayConst evs = root["events"];
  if (!evs.isNull()) {
    for (JsonObjectConst e : evs) {
      s.eventText = String((const char*)(e["text"] | "")).substring(0, FIELD_MAX);
      s.eventAt   = millis();
      if (s.eventText.length()) s.busyText = "";   // an alert supersedes the spinner
    }
  }

  s.lastPushMs  = millis();
  lastAnyPushMs = millis();
  everAnyPushed = true;

  reply("{\"ok\":true}");
  drawScreen();
}

void pumpSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\n' || c == '\r') {
      if (lineOverflow) { reply("{\"error\":\"line too long\"}"); lineOverflow = false; }
      else if (lineBuf.length()) handleLine(lineBuf);
      lineBuf = "";
      continue;
    }

    if (lineBuf.length() >= LINE_MAX) {
      // Drop the rest of the line rather than growing without bound.
      lineOverflow = true;
      lineBuf = "";
      continue;
    }
    lineBuf += c;
  }
}

/** Any reason to redraw faster than the 1 Hz baseline? Read-only - unlike
 *  pickSlotToShow()/drawQuotaRow(), never advances a rotation clock, so
 *  loop() can call it freely without corrupting either 3-second timing. */
bool anyNeedsFastCadence() {
  int used[MAX_SESSIONS];
  int n = listUsed(used);
  if (n == 0) return everAnyPushed;   // the m==0 idle graphic animates too
  if (n > 1) return true;             // rotation needs checking reasonably promptly
  for (int i = 0; i < n; i++) {
    if (slotBusyLive(sessions[used[i]]) || slotStale(sessions[used[i]])) return true;
  }
  return false;
}

// ── Setup / loop ──────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  lineBuf.reserve(256);

#ifdef DISPLAY_SPI
  // SPI is write-only: there is no ACK, so begin() succeeding proves the driver
  // initialised, NOT that a display is attached or wired correctly.
  displayOk = display.begin(0, true);
  logf("oled SPI: MOSI=%d CLK=%d DC=%d RST=%d CS=%d", OLED_MOSI, OLED_CLK, OLED_DC, OLED_RST, OLED_CS);
#else
  Wire.begin(SDA_PIN, SCL_PIN);
  displayOk = display.begin(OLED_ADDR, true);
  if (!displayOk) {
    logf("oled not found at 0x%02X - continuing headless", OLED_ADDR);
    logf("check wiring: SDA=GPIO%d SCL=GPIO%d VCC=3.3V", SDA_PIN, SCL_PIN);
  }
#endif

  if (displayOk) { display.clearDisplay(); display.display(); }

  logf("Clauled %s ready - reading JSON lines from USB serial", CLAULED_VERSION);
  drawScreen();
  lastDraw = millis();
}

void loop() {
  pumpSerial();

  // Once powered down there is nothing to animate, so drop back to 1 Hz
  // rather than polling uselessly.
  const bool animating = !displayAsleep && anyNeedsFastCadence();
  if (millis() - lastDraw >= (animating ? 300UL : 1000UL)) {
    drawScreen();
    lastDraw = millis();
  }

  // BOOT clears whichever session's banner/spinner is CURRENTLY ON SCREEN,
  // not every session's - dismissing what you are looking at should not
  // silently dismiss two other sessions' "Your turn" you have not seen yet.
  // Also forces one frame even during a quiet-hours power-down - see
  // drawScreen(force).
  static bool lastBtn = HIGH;
  bool btn = digitalRead(BOOT_BTN);
  if (btn == LOW && lastBtn == HIGH) {
    if (lastShownSlot >= 0 && lastShownSlot < MAX_SESSIONS && sessions[lastShownSlot].used) {
      sessions[lastShownSlot].eventText = "";
      sessions[lastShownSlot].busyText  = "";
    }
    drawScreen(true);
    delay(200);
  }
  lastBtn = btn;
}
