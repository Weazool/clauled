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
String  footerL = "";

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

void drawGauge(int labelY, int barY, const String& label, float pct) {
  String pctStr = (pct >= 0) ? String((int)(pct + 0.5f)) + "%" : "--";
  drawLR(labelY, label.length() ? label : "Usage", pctStr);
  display.drawRect(0, barY, SCREEN_W, 6, SH110X_WHITE);
  if (pct >= 0) {
    int fill = (int)((SCREEN_W - 2) * constrain(pct, 0.0f, 100.0f) / 100.0f);
    if (fill > 0) display.fillRect(1, barY + 1, fill, 4, SH110X_WHITE);
  }
}

// Inverted banner: white block, black text. Deliberately the loudest thing on
// the screen - it is the one state meant to catch your eye across the room.
void drawBanner(int y, const String& text) {
  display.fillRect(0, y - 1, SCREEN_W, 9, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  String t = text.substring(0, LINE_CHARS);
  int px = (SCREEN_W - (int)t.length() * CHAR_W) / 2;
  display.setCursor(px < 0 ? 0 : px, y);
  display.print(t);
  display.setTextColor(SH110X_WHITE);
}

// Spinner turns on the device, so a long turn keeps moving with no pushes.
void drawBusy(int y, const String& text) {
  static const char frames[] = {'|', '/', '-', '\\'};
  char c = frames[(millis() / 300) % 4];
  display.setCursor(0, y);
  display.print(c);
  display.setCursor(2 * CHAR_W, y);
  display.print(text.substring(0, LINE_CHARS - 2));
}

void drawSleep() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);

  drawLR(0, "Claude", title);
  display.drawLine(0, 9, 127, 9, SH110X_WHITE);

  display.setCursor(8, 30);
  display.print("(-_-)");

  const unsigned long f = millis() / 300;
  for (int i = 0; i < 3; i++) {
    int phase = (int)((f + i * 3) % 9);
    if (phase > 5) continue;                 // a gap, so they don't crowd
    display.setTextSize(1 + phase / 2);
    display.setCursor(44 + phase * 10, 44 - phase * 6);
    display.print("Z");
  }
  display.setTextSize(1);

  display.drawLine(0, 52, 127, 52, SH110X_WHITE);
  drawLR(54, "asleep " + fmtAge(millis() - lastPushMs), "USB idle");
  display.display();
}

void drawScreen() {
  if (!displayOk) return;

  // Nothing for a while: the host is asleep or Claude Code is closed. The
  // device cannot tell which, and does not pretend to.
  if (everPushed && dataStale()) { drawSleep(); return; }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  if (!everPushed) {
    drawLR(0, "Claude", "v" CLAULED_VERSION);
    display.drawLine(0, 9, 127, 9, SH110X_WHITE);
    String a = "Waiting for data";
    String b = "over USB serial";
    display.setCursor((SCREEN_W - (int)a.length() * CHAR_W) / 2, 24);
    display.print(a);
    display.setCursor((SCREEN_W - (int)b.length() * CHAR_W) / 2, 36);
    display.print(b);
    display.drawLine(0, 52, 127, 52, SH110X_WHITE);
    drawLR(54, "no data", "USB idle");
    display.display();
    return;
  }

  drawLR(0, "Claude", title);
  display.drawLine(0, 9, 127, 9, SH110X_WHITE);

  drawGauge(11, 20, g1Label, g1Pct);
  drawGauge(27, 36, g2Label, g2Pct);

  // Detail row resolves in priority order: an alert always wins over a
  // spinner, and a spinner over the static detail.
  if      (eventLive()) drawBanner(44, eventText);
  else if (busyLive())  drawBusy(44, busyText);
  else                  drawLR(44, rowL, rowR);

  display.drawLine(0, 52, 127, 52, SH110X_WHITE);
  drawLR(54, footerL, "USB ok");

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
  if (!root["title"].isNull()) title = field(root["title"], title);
  applyGauge(root["gauge1"], g1Label, g1Pct);
  applyGauge(root["gauge2"], g2Label, g2Pct);

  JsonVariantConst r = root["row"];
  if (!r.isNull()) {
    rowL = field(r["left"],  rowL);
    rowR = field(r["right"], rowR);
  }

  JsonVariantConst f = root["footer"];
  if (!f.isNull()) footerL = field(f["left"], footerL);

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
