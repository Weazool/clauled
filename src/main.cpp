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
//    0- 7   header  session               model
//    9      rule
//   11-18   text    "5h reset"   "4h25m"     "7%"
//   20-25   bar     quota                        (6 px)
//   27-34   text    "ctx"      "357k/1M"    "45%"
//   36-41   bar     context                      (6 px)
//   43-53   status  banner / spinner / sleep     (invertible)
//   55      rule
//   56-63   footer  cost                  effort
#define HEADER_Y       0
#define HEAD_RULE_Y    9
#define ROW1_Y        11
#define BAR1_Y        20
#define ROW2_Y        27
#define BAR2_Y        36
#define BAR_H          6
#define STATUS_Y      43
#define STATUS_H      11
#define STATUS_TEXT_Y 45     // 7-px glyph centred in the 11-px band
#define RULE_Y        55
#define BOTTOM_Y      56

// ── Protocol limits ───────────────────────────────────────────
// A malformed or hostile line must not be able to exhaust heap.
#define SCHEMA_VERSION   3
#define LINE_MAX         2048
#define FIELD_MAX        21     // one screen line
#define EVENT_TTL_S      300
#define BUSY_TTL_S       180    // a missed Stop hook must not spin forever

#ifdef DISPLAY_SPI
// Software SPI: works on any GPIOs with no peripheral setup, and the 1 KB
// framebuffer at ~3 Hz makes bitbang speed a non-issue.
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, OLED_MOSI, OLED_CLK, OLED_DC, OLED_RST, OLED_CS);
#else
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);
#endif

// ── Pushed state ──────────────────────────────────────────────
String  title = "";
String  g1Label = "", g2Label = "";
float   g1Pct = -1, g2Pct = -1;
String  rowL = "", rowR = "";
String  footerL = "", footerR = "";
String  session = "";

String        busyText = "";
unsigned long busyAt   = 0;
String        eventText = "";
unsigned long eventAt   = 0;

unsigned long lastPushMs = 0;
bool          everPushed = false;

// ── Runtime ───────────────────────────────────────────────────
unsigned long lastDraw  = 0;
bool          displayOk = false;
String        lineBuf;
bool          lineOverflow = false;

// ── Helpers ───────────────────────────────────────────────────
String fmtAge(unsigned long ms) {
  unsigned long s = ms / 1000;
  if (s < 60)   return String(s) + "s";
  if (s < 3600) return String(s / 60) + "m";
  return String(s / 3600) + "h";
}

bool dataStale() {
  if (!everPushed) return true;
  return ((millis() - lastPushMs) / 1000UL) > (unsigned long)STALE_AFTER_S;
}

bool eventLive() {
  if (eventText.length() == 0) return false;
  return ((millis() - eventAt) / 1000UL) <= (unsigned long)EVENT_TTL_S;
}

bool busyLive() {
  if (busyText.length() == 0) return false;
  return ((millis() - busyAt) / 1000UL) <= (unsigned long)BUSY_TTL_S;
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

// Inverted banner: white block, black text. Deliberately the loudest thing on
// the screen - it is the one state meant to catch your eye across the room.
void drawBanner(const String& text) {
  display.fillRect(0, STATUS_Y, SCREEN_W, STATUS_H, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  String t = text.substring(0, LINE_CHARS);
  int px = (SCREEN_W - (int)t.length() * CHAR_W) / 2;
  display.setCursor(px < 0 ? 0 : px, STATUS_TEXT_Y);
  display.print(t);
  display.setTextColor(SH110X_WHITE);
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
 * The old animation took the whole screen and grew its Z's from size 1 to 3.
 * There is no room for that in an 11-pixel row - a size-2 glyph is already 16
 * pixels tall - so what is left is a horizontal march with a slight rise. The
 * gauges surviving is worth more than the animation was: overnight is exactly
 * when you glance over casually, and the old behaviour showed nothing at all.
 */
void drawSleepRow() {
  display.setCursor(0, STATUS_TEXT_Y);
  display.print("(-_-)");
  display.setCursor(6 * CHAR_W, STATUS_TEXT_Y);
  display.print(fmtAge(millis() - lastPushMs));

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

void drawScreen() {
  if (!displayOk) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  if (!everPushed) {
    String a = "Waiting for data";
    String b = "over USB serial";
    display.setCursor((SCREEN_W - (int)a.length() * CHAR_W) / 2, 20);
    display.print(a);
    display.setCursor((SCREEN_W - (int)b.length() * CHAR_W) / 2, 32);
    display.print(b);
    display.drawLine(0, RULE_Y, SCREEN_W - 1, RULE_Y, SH110X_WHITE);
    drawLR(BOTTOM_Y, "Clauled", "v" CLAULED_VERSION);
    display.display();
    return;
  }

  // Header: which session on the left, which model on the right. The session
  // is truncated to whatever the model leaves free, never the other way round -
  // the model is short and fixed, the session name is neither.
  drawLR(HEADER_Y, session, title);
  display.drawLine(0, HEAD_RULE_Y, SCREEN_W - 1, HEAD_RULE_Y, SH110X_WHITE);

  // Each gauge is a three-column line and a bar. The row detail pairs with its
  // gauge: rowL is the quota's reset countdown, rowR the context's token counts.
  drawRow3(ROW1_Y, g1Label, rowL, pctText(g1Pct));
  drawBar(BAR1_Y, g1Pct);

  drawRow3(ROW2_Y, g2Label, rowR, pctText(g2Pct));
  drawBar(BAR2_Y, g2Pct);

  // Status row resolves in priority order: an alert always wins over a
  // spinner, a spinner over sleep. Nothing at all is a legitimate state - it
  // means a turn ended more than EVENT_TTL_S ago but the host is still alive.
  if      (eventLive()) drawBanner(eventText);
  else if (busyLive())  drawBusy(busyText);
  else if (everPushed && dataStale()) drawSleepRow();

  display.drawLine(0, RULE_Y, SCREEN_W - 1, RULE_Y, SH110X_WHITE);

  // Cost left, effort right. The USB indicator that used to sit here is gone:
  // it could never tell "Claude Code closed" from "cable unplugged", and the
  // status row above already says whether anything is happening.
  drawLR(BOTTOM_Y, footerL, footerR);

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
  JsonDocument doc;
  doc["ok"]            = true;
  doc["version"]       = CLAULED_VERSION;
  doc["display_ok"]    = displayOk;
  doc["uptime"]        = millis() / 1000;
  doc["last_push_age"] = everPushed ? (long)((millis() - lastPushMs) / 1000UL) : -1;
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

  // Merge, never replace: a hook pushing only "busy" must not wipe the gauges,
  // and the statusline pushing only gauges must not clear the busy state.
  if (!root["title"].isNull())   title   = field(root["title"], title);
  if (!root["session"].isNull()) session = field(root["session"], session);
  applyGauge(root["gauge1"], g1Label, g1Pct);
  applyGauge(root["gauge2"], g2Label, g2Pct);

  JsonVariantConst r = root["row"];
  if (!r.isNull()) {
    rowL = field(r["left"],  rowL);
    rowR = field(r["right"], rowR);
  }

  JsonVariantConst f = root["footer"];
  if (!f.isNull()) {
    footerL = field(f["left"],  footerL);
    footerR = field(f["right"], footerR);
  }

  // An empty string clears the spinner - that is how Stop ends a turn.
  if (!root["busy"].isNull()) {
    busyText = field(root["busy"], "");
    busyAt   = millis();
    // Going busy also clears any banner: if Claude is working, it is not your
    // turn. Without this a "Your turn" banner outranks every spinner beneath it
    // for its full TTL, and the display looks frozen mid-task.
    if (busyText.length()) eventText = "";
  }

  JsonArrayConst evs = root["events"];
  if (!evs.isNull()) {
    for (JsonObjectConst e : evs) {
      eventText = String((const char*)(e["text"] | "")).substring(0, FIELD_MAX);
      eventAt   = millis();
      if (eventText.length()) busyText = "";   // an alert supersedes the spinner
    }
  }

  lastPushMs = millis();
  everPushed = true;

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

  // The spinner and the sleep animation need a faster cadence than the once-a
  // second the footer would otherwise require.
  const bool animating = (everPushed && dataStale()) || busyLive();
  if (millis() - lastDraw >= (animating ? 300UL : 1000UL)) {
    drawScreen();
    lastDraw = millis();
  }

  // BOOT button clears a stuck banner or spinner early.
  static bool lastBtn = HIGH;
  bool btn = digitalRead(BOOT_BTN);
  if (btn == LOW && lastBtn == HIGH) {
    eventText = "";
    busyText  = "";
    drawScreen();
    delay(200);
  }
  lastBtn = btn;
}
