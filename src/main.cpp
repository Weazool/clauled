// ============================================================
//  Clauled - Claude usage display for ESP32-C3 + SH1106 OLED
//
//  This device holds NO Claude credentials and never contacts
//  Anthropic. A pusher on your desktop POSTs usage and events to
//  http://clauled.local/push over the LAN; the device just renders
//  whatever it was last told. See API.md for the wire contract.
//
//  Configuration lives in src/secrets.h (gitignored).
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <vector>

#include "secrets.h"
#include "version.h"

// Fail at build time rather than booting into a mode that no longer exists.
static_assert(WIFI_SSID[0] != '\0',
  "Set WIFI_SSID in src/secrets.h (copy src/secrets.h.example)");
static_assert(WIFI_PASSWORD[0] != '\0',
  "Set WIFI_PASSWORD in src/secrets.h");
static_assert(CLAULED_PUSH_KEY[0] != '\0',
  "Set CLAULED_PUSH_KEY in src/secrets.h - the push endpoint must not be unauthenticated");

// ── Pins ──────────────────────────────────────────────────────
#define SDA_PIN   4
#define SCL_PIN   5
#define BOOT_BTN  9

// ── Display ───────────────────────────────────────────────────
#define OLED_ADDR  0x3C
#define SCREEN_W   128
#define SCREEN_H   64

// ── Push limits ───────────────────────────────────────────────
// A malformed or hostile push must not be able to exhaust heap.
#define SCHEMA_VERSION   1
#define PUSH_MAX_BODY    2048
#define MAX_EVENTS       8
#define EVENT_TEXT_MAX   40
#define EVENT_TTL_S      300

Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);
WebServer server(80);

// ── Pushed state ──────────────────────────────────────────────
// resets_in arrives as seconds remaining, not an absolute time, so the
// device needs no wall clock: we stamp millis() on receipt and count down
// locally between pushes.
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

unsigned long lastPushMs  = 0;
bool          everPushed  = false;
String        deviceIP    = "";

// ── Runtime ───────────────────────────────────────────────────
int           currentPage = 0;
int           pageCount   = 0;
unsigned long lastCycle   = 0;
unsigned long lastDraw    = 0;

// A missing display must not take the device down with it. Serving /push and
// /health is the primary job; the screen is an output device for it.
bool          displayOk   = false;

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

// Constant-time over the compared bytes. The length check leaks only the
// key length, which is not secret.
bool pushKeyOk(const String& given) {
  const char* expect = CLAULED_PUSH_KEY;
  size_t elen = strlen(expect);
  if (given.length() != elen) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < elen; i++) diff |= (uint8_t)(given[i] ^ expect[i]);
  return diff == 0;
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

  // Left: how long since the last accepted push. Stale is a normal
  // overnight state (the pusher only runs while Claude Code is open),
  // so it reads as information, not as an error.
  display.setCursor(0, 57);
  String age = everPushed ? fmtAge(millis() - lastPushMs) : "no data";
  display.print(dataStale() ? ("stale " + age) : age);

  // Right: IP, still the quickest confirmation the device is on the network.
  if (deviceIP.length()) {
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(deviceIP, 0, 57, &x1, &y1, &w, &h);
    display.setCursor(128 - (int)w, 57);
    display.print(deviceIP);
  }
}

void drawScreen() {
  if (!displayOk) return;
  buildPages();
  display.clearDisplay();

  if (!everPushed) {
    oledStatus("Waiting for data", CLAULED_HOSTNAME ".local", deviceIP.c_str());
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

// ── HTTP handlers ─────────────────────────────────────────────
void mergeMetric(Metric& m, JsonVariantConst j) {
  if (j.isNull()) return;
  if (!j["pct"].isNull())       { m.pct = j["pct"].as<float>(); m.known = true; }
  if (!j["resets_in"].isNull()) { m.resetsIn = j["resets_in"].as<long>(); }
  m.stampedAt = millis();
}

void handlePush() {
  if (!pushKeyOk(server.header("X-Clauled-Key"))) {
    Serial.println("[push] rejected: bad or missing key");
    server.send(401, "application/json", "{\"error\":\"bad key\"}");
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"no body\"}");
    return;
  }

  String body = server.arg("plain");
  if (body.length() > PUSH_MAX_BODY) {
    server.send(413, "application/json", "{\"error\":\"body too large\"}");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"bad JSON\"}");
    return;
  }

  // Read through a const view so a lookup can never insert into the document.
  JsonObjectConst root = doc.as<JsonObjectConst>();

  int v = root["v"] | SCHEMA_VERSION;
  if (v != SCHEMA_VERSION) {
    // Fail loudly rather than silently misreading a future schema.
    server.send(400, "application/json", "{\"error\":\"unsupported schema version\"}");
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

  Serial.printf("[push] ok  5h=%s  7d=%s\n",
    m5h.known ? String((int)m5h.pct).c_str() : "--",
    m7d.known ? String((int)m7d.pct).c_str() : "--");

  server.send(200, "application/json", "{\"ok\":true}");
  drawScreen();
}

// Unauthenticated on purpose: exposes nothing sensitive, and lets the
// pusher check reachability before it bothers building a payload.
void handleHealth() {
  JsonDocument doc;
  doc["ok"]            = true;
  doc["version"]       = CLAULED_VERSION;
  doc["display_ok"]    = displayOk;
  doc["uptime"]        = millis() / 1000;
  doc["last_push_age"] = everPushed ? (long)((millis() - lastPushMs) / 1000UL) : -1;
  doc["schema"]        = SCHEMA_VERSION;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

// ── Network ───────────────────────────────────────────────────
// Retry forever rather than rebooting: a desk gadget should self-heal
// through a router restart without human intervention.
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(CLAULED_HOSTNAME);

  unsigned int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    attempt++;
    Serial.printf("[wifi] connecting to '%s' (attempt %u)\n", WIFI_SSID, attempt);

    String line3 = "try " + String(attempt);
    oledStatus("Connecting...", WIFI_SSID, line3.c_str());

    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);

    if (WiFi.status() != WL_CONNECTED) {
      unsigned long backoff = min(30000UL, 2000UL * attempt);
      Serial.printf("[wifi] failed, retrying in %lums\n", backoff);
      oledStatus("WiFi failed", "Retrying...", WIFI_SSID);
      delay(backoff);
    }
  }

  deviceIP = WiFi.localIP().toString();
  Serial.printf("[wifi] connected, IP=%s\n", deviceIP.c_str());
  oledStatus("Connected!", deviceIP.c_str());
  delay(600);
}

// Re-callable: mDNS has to be restarted after a reconnect because the
// advertisement is bound to the old address.
void startMDNS() {
  MDNS.end();
  if (MDNS.begin(CLAULED_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mdns] http://%s.local/\n", CLAULED_HOSTNAME);
  } else {
    Serial.println("[mdns] failed to start (device still reachable by IP)");
  }
}

// Call once. Re-running server.on() would append duplicate handlers on
// every reconnect, so this deliberately stays out of the reconnect path.
void startServer() {
  // Required: without this the WebServer discards custom headers and
  // server.header("X-Clauled-Key") always comes back empty.
  static const char* headerKeys[] = { "X-Clauled-Key" };
  server.collectHeaders(headerKeys, 1);

  server.on("/push",   HTTP_POST, handlePush);
  server.on("/health", HTTP_GET,  handleHealth);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[http] listening on :80  (POST /push, GET /health)");
}

// ── Setup / loop ──────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(BOOT_BTN, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);
  displayOk = display.begin(OLED_ADDR, true);
  if (displayOk) {
    display.clearDisplay();
    display.display();
  } else {
    // Do NOT halt. A loose I2C wire must not cost you the network endpoint,
    // or the failure becomes diagnosable only over a USB cable.
    Serial.println("[oled] not found at 0x3C - continuing headless");
    Serial.println("[oled] check wiring: SDA=GPIO4, SCL=GPIO5, VCC=3.3V");
  }

  // Show what is actually flashed, so a stale board is obvious at a glance.
  Serial.printf("[boot] Clauled %s\n", CLAULED_VERSION);
  oledStatus("Clauled", "v" CLAULED_VERSION);
  if (displayOk) delay(1200);

  connectWiFi();
  startMDNS();
  startServer();

  drawScreen();
  lastCycle = millis();
  lastDraw  = millis();
}

void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] lost connection");
    connectWiFi();
    startMDNS();
    drawScreen();
  }

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
