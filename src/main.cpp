// ============================================================
//  Clauled - Claude usage display for ESP32-C3 + SH1106 OLED
//
//  The device has no network stack and holds no credentials. It reads
//  newline-delimited JSON from USB serial and renders it. A plugin on the
//  host writes to the serial port; see API.md for the protocol.
//
//  Configuration is in src/config.h (tracked - nothing secret).
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <vector>

#include "config.h"
#include "version.h"

// ── Pins ──────────────────────────────────────────────────────
#define BOOT_BTN  9

// ── Display ───────────────────────────────────────────────────
#define SCREEN_W   128
#define SCREEN_H   64

// ── Protocol limits ───────────────────────────────────────────
// A malformed or hostile line must not be able to exhaust heap.
#define SCHEMA_VERSION   1
#define LINE_MAX         2048
#define MAX_EVENTS       8
#define EVENT_TEXT_MAX   40
#define EVENT_TTL_S      300

#ifdef DISPLAY_SPI
// Software SPI: works on any GPIOs with no peripheral setup, and the 1 KB
// framebuffer at ~1 Hz makes bitbang speed a non-issue.
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, OLED_MOSI, OLED_CLK, OLED_DC, OLED_RST, OLED_CS);
#else
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);
#endif

// ── Pushed state ──────────────────────────────────────────────
// resets_in arrives as seconds remaining, not an absolute time, so the device
// needs no clock: we stamp millis() on receipt and count down locally.
struct Metric {
  bool          known     = false;
  float         pct       = -1;
  long          resetsIn  = -1;
  unsigned long stampedAt = 0;

  long remaining() const {
    if (resetsIn < 0) return -1;
    long elapsed = (long)((millis() - stampedAt) / 1000UL);
    long r = resetsIn - elapsed;
    return r > 0 ? r : 0;
  }
};

Metric m5h, m7d, m7dSonnet;

struct Event {
  String        type;
  String        text;
  unsigned long at = 0;
};
Event lastEvent;
bool  hasEvent = false;

unsigned long lastPushMs = 0;
bool          everPushed = false;

// ── Runtime ───────────────────────────────────────────────────
int           currentPage = 0;
int           pageCount   = 0;
unsigned long lastCycle   = 0;
unsigned long lastDraw    = 0;

// A missing display must not take the device down with it.
bool          displayOk   = false;

// Serial line assembly
String        lineBuf;
bool          lineOverflow = false;

// ── Page layout ───────────────────────────────────────────────
enum PageType { PAGE_BAR, PAGE_ROWS, PAGE_EVENT };
struct BarPage   { String title; String valText; int pct; };
struct RowPage   { struct Row { String label; String value; }; std::vector<Row> rows; };
struct EventPage { String title; String text; };
struct Page      { PageType type; BarPage bar; RowPage rows; EventPage event; };
std::vector<Page> pages;

// ── Helpers ───────────────────────────────────────────────────
String fmtUptime() {
  unsigned long s = millis() / 1000;
  char buf[9];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
           (s / 3600) % 24, (s / 60) % 60, s % 60);
  return String(buf);
}

// "2d 3h" / "1h 21m" / "45m" / "30s" / "now" / "--"
String fmtCountdown(long secs) {
  if (secs <  0) return "--";
  if (secs == 0) return "now";
  long d = secs / 86400;
  long h = (secs % 86400) / 3600;
  long m = (secs % 3600) / 60;
  if (d > 0) return String(d) + "d " + String(h) + "h";
  if (h > 0) return String(h) + "h " + String(m) + "m";
  if (m > 0) return String(m) + "m";
  return String(secs) + "s";
}

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
  if (!hasEvent) return false;
  return ((millis() - lastEvent.at) / 1000UL) <= (unsigned long)EVENT_TTL_S;
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

// ── Page builder ─────────────────────────────────────────────
void addBarPage(const String& title, const String& val, int p) {
  Page pg; pg.type = PAGE_BAR; pg.bar = {title, val, p};
  pages.push_back(pg);
}

void addRowsPage(std::vector<RowPage::Row> rows) {
  Page pg; pg.type = PAGE_ROWS; pg.rows.rows = rows;
  pages.push_back(pg);
}

void addEventPage(const String& title, const String& text) {
  Page pg; pg.type = PAGE_EVENT; pg.event = {title, text};
  pages.push_back(pg);
}

void buildPages() {
  pages.clear();

  auto pctOf   = [](const Metric& m) -> int {
    return m.known ? (int)constrain(m.pct, 0.0f, 100.0f) : -1;
  };
  auto resetOf = [](const Metric& m) -> String {
    long r = m.remaining();
    return (r < 0) ? "--" : ("Resets in " + fmtCountdown(r));
  };

  addBarPage("Current session",   resetOf(m5h), pctOf(m5h));
  addBarPage("Weekly all models", resetOf(m7d), pctOf(m7d));

  if (SHOW_WEEKLY_SONNET) {
    addBarPage("Weekly Sonnet", resetOf(m7dSonnet), pctOf(m7dSonnet));
  }

  if (eventLive()) {
    addEventPage(lastEvent.type.length() ? lastEvent.type : "Event", lastEvent.text);
  }

  if (SHOW_UPTIME) {
    addRowsPage({{"Uptime", fmtUptime()}});
  }

  pageCount = pages.size();
  if (currentPage >= pageCount) currentPage = 0;
}

// ── OLED rendering ────────────────────────────────────────────
void oledStatus(const char* l1, const char* l2 = "", const char* l3 = "") {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 8);  display.print(l1);
  display.setCursor(0, 26); display.print(l2);
  display.setCursor(0, 44); display.print(l3);
  display.display();
}

void drawTextRow(int y, const String& label, const String& value) {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, y);
  display.print(label.substring(0, 13));
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(value, 0, y, &x1, &y1, &w, &h);
  display.setCursor(128 - (int)w, y);
  display.print(value);
}

int drawBar(int y, const String& title, const String& valText, int p) {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  int16_t x1, yy1; uint16_t tw, th;

  String pctStr = (p >= 0) ? String(p) + "%" : "--";
  display.getTextBounds(pctStr, 0, y, &x1, &yy1, &tw, &th);
  display.setCursor(128 - (int)tw, y);
  display.print(pctStr);

  display.setCursor(0, y);
  int maxChars = max(1, (128 - (int)tw - 4) / 6);
  display.print(title.substring(0, maxChars));
  y += 10;

  display.drawRect(0, y, 128, 6, SH110X_WHITE);
  if (p >= 0) {
    int fillW = (int)(128L * p / 100);
    if (fillW > 0) display.fillRect(0, y, fillW, 6, SH110X_WHITE);
  }
  y += 8;

  display.getTextBounds(valText, 0, y, &x1, &yy1, &tw, &th);
  display.setCursor((128 - (int)tw) / 2, y);
  display.print(valText);
  y += 10;
  return y;
}

// Naive word wrap at 21 chars/line (6px font, 128px wide).
void drawWrapped(int y, const String& text, int maxLines) {
  const int perLine = 21;
  String rest = text;
  for (int line = 0; line < maxLines && rest.length(); line++) {
    String chunk;
    if ((int)rest.length() <= perLine) {
      chunk = rest;
      rest  = "";
    } else {
      int cut = rest.lastIndexOf(' ', perLine);
      if (cut <= 0) cut = perLine;
      chunk = rest.substring(0, cut);
      rest  = rest.substring(cut);
      rest.trim();
    }
    display.setCursor(0, y);
    display.print(chunk);
    y += 11;
  }
}

void drawHeader(int page, int total) {
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.print("Claude");
  if (total > 1) {
    String pg = String(page + 1) + "/" + String(total);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(pg, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(128 - (int)w, 0);
    display.print(pg);
  }
  display.drawLine(0, 9, 127, 9, SH110X_WHITE);
}

void drawFooter() {
  display.drawLine(0, 54, 127, 54, SH110X_WHITE);
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  // Left: time since the last accepted push. Stale is normal when the host is
  // asleep or Claude Code is closed, so it reads as information, not an error.
  display.setCursor(0, 57);
  String age = everPushed ? fmtAge(millis() - lastPushMs) : "no data";
  display.print(dataStale() ? ("stale " + age) : age);

  // Right: the link is USB, and it is either carrying data or it is not.
  String right = everPushed && !dataStale() ? "USB ok" : "USB idle";
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(right, 0, 57, &x1, &y1, &w, &h);
  display.setCursor(128 - (int)w, 57);
  display.print(right);
}

void drawScreen() {
  if (!displayOk) return;
  buildPages();
  display.clearDisplay();

  if (!everPushed) {
    oledStatus("Waiting for data", "over USB serial", "v" CLAULED_VERSION);
    return;
  }

  Page& pg = pages[currentPage];
  drawHeader(currentPage, pageCount);

  if (pg.type == PAGE_BAR) {
    drawBar(12, pg.bar.title, pg.bar.valText, pg.bar.pct);
  } else if (pg.type == PAGE_EVENT) {
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 13);
    display.print(pg.event.title.substring(0, 21));
    drawWrapped(26, pg.event.text, 2);
  } else {
    int y = 13;
    for (auto& row : pg.rows.rows) {
      drawTextRow(y, row.label, row.value);
      y += 14;
    }
  }

  drawFooter();
  display.display();
}

// ── Protocol ──────────────────────────────────────────────────
void reply(const char* json) {
  Serial.println(json);
}

void mergeMetric(Metric& m, JsonVariantConst j) {
  if (j.isNull()) return;
  if (!j["pct"].isNull())       { m.pct = j["pct"].as<float>(); m.known = true; }
  if (!j["resets_in"].isNull()) { m.resetsIn = j["resets_in"].as<long>(); }
  m.stampedAt = millis();
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
  if (deserializeJson(doc, line)) {
    reply("{\"error\":\"bad JSON\"}");
    return;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();

  int v = root["v"] | SCHEMA_VERSION;
  if (v != SCHEMA_VERSION) {
    reply("{\"error\":\"unsupported schema version\"}");
    return;
  }

  // Status probe: lets the host confirm it is talking to a Clauled device
  // on this port, without changing anything.
  const char* cmd = root["cmd"] | "";
  if (strcmp(cmd, "status") == 0) {
    sendStatus();
    return;
  }

  // Merge, never replace: a push carrying only events must not wipe the bars.
  JsonVariantConst usage = root["usage"];
  mergeMetric(m5h,       usage["five_hour"]);
  mergeMetric(m7d,       usage["seven_day"]);
  mergeMetric(m7dSonnet, usage["seven_day_sonnet"]);

  JsonArrayConst evs = root["events"];
  if (!evs.isNull()) {
    int n = 0;
    for (JsonObjectConst e : evs) {
      if (++n > MAX_EVENTS) break;
      lastEvent.type = String((const char*)(e["type"] | "")).substring(0, 21);
      lastEvent.text = String((const char*)(e["text"] | "")).substring(0, EVENT_TEXT_MAX);
      lastEvent.at   = millis();
      hasEvent       = true;
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
      if (lineOverflow) {
        reply("{\"error\":\"line too long\"}");
        lineOverflow = false;
      } else if (lineBuf.length()) {
        handleLine(lineBuf);
      }
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
  // initialised, NOT that a display is attached or wired correctly. Only seeing
  // pixels confirms that.
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

  if (displayOk) {
    display.clearDisplay();
    display.display();
  }

  logf("Clauled %s ready - reading JSON lines from USB serial", CLAULED_VERSION);
  oledStatus("Clauled", "v" CLAULED_VERSION);
  delay(1000);

  drawScreen();
  lastCycle = millis();
  lastDraw  = millis();
}

void loop() {
  pumpSerial();

  // Countdowns tick locally between pushes, so redraw once a second.
  if (millis() - lastDraw >= 1000UL) {
    drawScreen();
    lastDraw = millis();
  }

  if (CYCLE_TIME > 0 && pageCount > 1
      && millis() - lastCycle >= (unsigned long)CYCLE_TIME * 1000UL) {
    currentPage = (currentPage + 1) % pageCount;
    drawScreen();
    lastCycle = millis();
  }

  static bool lastBtn = HIGH;
  bool btn = digitalRead(BOOT_BTN);
  if (btn == LOW && lastBtn == HIGH) {
    currentPage = (currentPage + 1) % max(1, pageCount);
    drawScreen();
    delay(200);
  }
  lastBtn = btn;
}
