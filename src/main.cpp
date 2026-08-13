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
#include <esp_wifi.h>
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
// Override SDA_PIN / SCL_PIN in secrets.h to move the display to other GPIOs.
// Defaults apply when secrets.h predates this option.
//
// Safe on ESP32-C3: 0, 1, 3, 4, 5, 6, 7, 10 (and 20/21, the UART pins, since
// serial runs over USB). Avoid 2 and 8 (boot strapping), 9 (BOOT button),
// 11-17 (SPI flash) and 18/19 (USB D-/D+ - using them costs you serial and
// flashing).
#ifndef SDA_PIN
#define SDA_PIN   4
#endif
#ifndef SCL_PIN
#define SCL_PIN   5
#endif

// SPI wiring, used when DISPLAY_SPI is defined in secrets.h (7-pin modules).
#ifndef OLED_MOSI
#define OLED_MOSI 4    // module pin "SDA" or "D1"
#endif
#ifndef OLED_CLK
#define OLED_CLK  5    // module pin "SCK" or "D0"
#endif
#ifndef OLED_DC
#define OLED_DC   6
#endif
#ifndef OLED_RST
#define OLED_RST  7
#endif
#ifndef OLED_CS
#define OLED_CS   10
#endif

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

#ifdef DISPLAY_SPI
// Software SPI: works on any GPIOs with no peripheral setup, and the 1 KB
// framebuffer at ~1 Hz makes bitbang speed a non-issue.
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, OLED_MOSI, OLED_CLK, OLED_DC, OLED_RST, OLED_CS);
#else
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);
#endif

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

// Last WiFi diagnosis, kept on screen through the retry backoff. A generic
// "retrying" tells you nothing you can act on.
String        wifiDiag2   = "scanning...";
String        wifiDiag3   = "";

// Network is optional. These track it without ever blocking the render loop.
#define WIFI_RETRY_MS 30000UL
bool          netUp        = false;   // currently associated + services running
bool          serverStarted = false;  // server.on() must only ever run once
unsigned long lastWifiTry  = 0;

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

// ── WiFi diagnostics ──────────────────────────────────────────
// The AP tells us exactly why it rejected us. Without this you are guessing
// between a wrong password, a router refusing the client, and being out of
// range - which need completely different fixes.
volatile int lastWifiReason = 0;

const char* wifiReasonName(int r) {
  switch (r) {
    case 2:   return "auth expired";
    case 4:   return "assoc expired";
    case 5:   return "AP client limit";     // router is full
    case 6:   return "not authed";
    case 7:   return "not assoced";
    case 8:   return "AP kicked us";
    case 15:  return "wrong password";      // 4-way handshake timeout
    case 200: return "beacon timeout";
    case 201: return "AP not found";
    case 202: return "auth failed";
    case 203: return "assoc failed";
    case 204: return "handshake timeout";
    case 0:   return "none yet";
    default:  return "see reason code";
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWifiReason = info.wifi_sta_disconnected.reason;
    Serial.printf("[wifi] DISCONNECT reason=%d (%s)\n",
                  lastWifiReason, wifiReasonName(lastWifiReason));
  }
}

// WPA3 / WPA2-WPA3 mixed is a well known cause of auth failures on ESP32 even
// with a perfect signal and a correct password, so report what the AP offers.
const char* wifiAuthName(int a) {
  switch (a) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ent";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "unknown";
  }
}

// Dump what the driver actually holds. Password LENGTH only - never the value.
// A length that is not what you typed means it was mangled before it ever
// reached the radio, which is a very different bug from the AP rejecting it.
void logStaConfig() {
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
    Serial.printf("[wifi] cfg ssid='%s' (len=%u)\n",
                  (const char*)conf.sta.ssid, (unsigned)strlen((const char*)conf.sta.ssid));
    Serial.printf("[wifi] cfg password length=%u  (expected %u)\n",
                  (unsigned)strlen((const char*)conf.sta.password),
                  (unsigned)strlen(WIFI_PASSWORD));
    Serial.printf("[wifi] cfg pmf capable=%d required=%d  min_authmode=%d (%s)\n",
                  conf.sta.pmf_cfg.capable, conf.sta.pmf_cfg.required,
                  conf.sta.threshold.authmode, wifiAuthName(conf.sta.threshold.authmode));
  }
  uint8_t mac[6];
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
    Serial.printf("[wifi] our MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
}

const char* wifiStatusName(int s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SSID_AVAIL:   return "SSID not found";
    case WL_SCAN_COMPLETED:  return "scan done";
    case WL_CONNECTED:       return "connected";
    case WL_CONNECT_FAILED:  return "auth failed";
    case WL_CONNECTION_LOST: return "conn lost";
    case WL_DISCONNECTED:    return "disconnected";
    default:                 return "unknown";
  }
}

// Distinguishes "can't see the network" (range, antenna, wrong band) from
// "can see it but was rejected" (password, router policy). Those need
// completely different fixes, and guessing between them wastes time.
void diagnoseWiFi() {
  int st = WiFi.status();
  Serial.printf("[wifi] status=%d (%s)\n", st, wifiStatusName(st));

  // A scan cannot run while an association attempt is still in flight - it
  // returns WIFI_SCAN_FAILED (-2). Stop the attempt first, or the result is
  // not "nothing visible", it is "we did not manage to look".
  WiFi.disconnect(false, false);
  delay(250);

  Serial.println("[wifi] scanning for visible networks...");
  int n = WiFi.scanNetworks();

  if (n < 0) {
    // -1 = still running, -2 = failed. Never report visibility from these.
    Serial.printf("[wifi] scan FAILED (%d) - visibility unknown, not concluding anything\n", n);
    wifiDiag2 = "scan failed";
    wifiDiag3 = wifiStatusName(st);
    oledStatus("WiFi problem", wifiDiag2.c_str(), wifiDiag3.c_str());
    return;
  }

  bool found = false;
  int32_t rssi = 0;
  int32_t chan = 0;

  int targetAuth = -1;
  int matches = 0;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) {
      matches++;
      found = true; rssi = WiFi.RSSI(i); chan = WiFi.channel(i);
      targetAuth = WiFi.encryptionType(i);
      Serial.printf("[wifi] target BSSID %s auth=%s\n",
                    WiFi.BSSIDstr(i).c_str(), wifiAuthName(targetAuth));
    }
  }
  if (matches > 1) Serial.printf("[wifi] NOTE: %d APs share this SSID (mesh/repeater)\n", matches);

  Serial.printf("[wifi] %d network(s) visible\n", n);
  for (int i = 0; i < n && i < 8; i++) {
    Serial.printf("[wifi]   %-24s %4lddBm ch%-3ld %s\n",
                  WiFi.SSID(i).c_str(), (long)WiFi.RSSI(i), (long)WiFi.channel(i),
                  wifiAuthName(WiFi.encryptionType(i)));
  }
  if (targetAuth == WIFI_AUTH_WPA3_PSK || targetAuth == WIFI_AUTH_WPA2_WPA3_PSK) {
    Serial.println("[wifi] >>> AP advertises WPA3. Set the 2.4GHz band to WPA2-Personal (AES).");
  }

  char line2[26];
  if (found) {
    Serial.printf("[wifi] TARGET FOUND: RSSI=%lddBm ch=%ld -> signal is fine, so this is\n", (long)rssi, (long)chan);
    Serial.println("[wifi]   auth/router side: check the password or router client limits.");
    snprintf(line2, sizeof(line2), "RSSI %lddBm ch%ld", (long)rssi, (long)chan);
  } else {
    Serial.printf("[wifi] TARGET '%s' NOT VISIBLE -> range, antenna placement, or\n", WIFI_SSID);
    Serial.println("[wifi]   the SSID is 5GHz only. Keep wires clear of the board's antenna.");
    snprintf(line2, sizeof(line2), "SSID not visible");
  }

  WiFi.scanDelete();
  wifiDiag2 = line2;
  wifiDiag3 = wifiStatusName(st);
  oledStatus("WiFi problem", wifiDiag2.c_str(), wifiDiag3.c_str());
}

// ── Network ───────────────────────────────────────────────────
// Bounded, and returns whether it succeeded. The display must never be held
// hostage by the network: a router that refuses us should cost you pushes,
// not the whole device. loop() keeps retrying in the background.
bool connectWiFi(unsigned int maxAttempts) {
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);

#ifdef WIFI_MAC_OVERRIDE
  // Diagnostic only. If the AP accepts us with a different MAC but refuses the
  // real one, the router is filtering by MAC - which is invisible in most
  // router UIs when the filter is in "accept listed only" mode.
  {
    uint8_t mac[6] = WIFI_MAC_OVERRIDE;
    esp_wifi_stop();                       // MAC can only be set while stopped
    esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, mac);
    esp_wifi_start();
    Serial.printf("[wifi] MAC OVERRIDE -> %02X:%02X:%02X:%02X:%02X:%02X (%s)\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  e == ESP_OK ? "applied" : esp_err_to_name(e));
  }
#endif

  WiFi.setHostname(CLAULED_HOSTNAME);
  WiFi.setSleep(false);          // modem sleep hurts both association and latency
  // Off deliberately: the driver's auto-reconnect fires roughly every second
  // and races the retry loop below, hammering the AP hard enough to trip
  // flood protection on some routers. One retry policy, not two.
  WiFi.setAutoReconnect(false);

  unsigned int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < maxAttempts) {
    attempt++;
    Serial.printf("[wifi] connecting to '%s' (attempt %u)\n", WIFI_SSID, attempt);

    String line3 = "try " + String(attempt);
    oledStatus("Connecting...", WIFI_SSID, line3.c_str());

    // Never power-cycle the radio between retries. disconnect(true) switches
    // WiFi off entirely: it cold-starts the stack each attempt and makes the
    // AP see a client flapping on and off, which some routers start refusing.
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    if (attempt == 1) logStaConfig();

    // 20s, not 15s: association plus DHCP can legitimately take that long.
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);

    if (WiFi.status() != WL_CONNECTED) {
      // Scanning disrupts association, so diagnose sparingly.
      if (attempt == 1 || attempt % 6 == 0) diagnoseWiFi();

      // Clear the half-open attempt without powering the radio down.
      WiFi.disconnect(false, false);
      delay(200);

      unsigned long backoff = min(15000UL, 1000UL * attempt);
      Serial.printf("[wifi] failed, retrying in %lums\n", backoff);

      // Hold the diagnosis on screen for the whole backoff so it can actually
      // be read, instead of flashing past in a few seconds.
      char l1[24];
      snprintf(l1, sizeof(l1), "WiFi try %u", attempt);
      char l3[26];
      snprintf(l3, sizeof(l3), "%d %s", lastWifiReason, wifiReasonName(lastWifiReason));
      Serial.printf("[wifi] last reason=%d (%s)\n", lastWifiReason, wifiReasonName(lastWifiReason));
      oledStatus(l1, wifiDiag2.c_str(), l3);
      delay(backoff);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] not connected - carrying on, display stays live");
    deviceIP = "";
    return false;
  }

  deviceIP = WiFi.localIP().toString();
  Serial.printf("[wifi] connected, IP=%s\n", deviceIP.c_str());
  oledStatus("Connected!", deviceIP.c_str());
  delay(600);
  return true;
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

#ifndef DISPLAY_SPI
// ── I2C diagnostics ───────────────────────────────────────────
// Only runs when the display was not found. Answers "is anything on the bus
// at all, and are SDA/SCL the right way round" in one boot, instead of
// requiring a separate scanner sketch.
int i2cScan(int sda, int scl) {
  Wire.end();
  delay(50);
  Wire.begin(sda, scl);
  Wire.setClock(100000);   // slow clock: most tolerant of long/marginal wiring
  delay(50);

  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[i2c]   device responded at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) Serial.println("[i2c]   nothing responded");
  return found;
}

void diagnoseI2C() {
  Serial.printf("[i2c] scanning as wired (SDA=GPIO%d, SCL=GPIO%d)...\n", SDA_PIN, SCL_PIN);
  int normal = i2cScan(SDA_PIN, SCL_PIN);

  Serial.printf("[i2c] scanning swapped (SDA=GPIO%d, SCL=GPIO%d)...\n", SCL_PIN, SDA_PIN);
  int swapped = i2cScan(SCL_PIN, SDA_PIN);

  if (normal == 0 && swapped == 0) {
    Serial.println("[i2c] VERDICT: nothing on the bus either way.");
    Serial.println("[i2c]   -> check VCC is on 3.3V and GND is connected,");
    Serial.println("[i2c]      or the module may be SPI rather than I2C.");
  } else if (normal == 0 && swapped > 0) {
    Serial.println("[i2c] VERDICT: found ONLY with pins swapped - SDA and SCL are reversed.");
  } else {
    Serial.println("[i2c] VERDICT: bus is alive but nothing answered at 0x3C.");
    Serial.println("[i2c]   -> if a device appeared at 0x3D, change OLED_ADDR in src/main.cpp.");
  }

  // Restore the configured pin order before continuing.
  Wire.end();
  delay(50);
  Wire.begin(SDA_PIN, SCL_PIN);
}
#endif  // !DISPLAY_SPI

// ── Setup / loop ──────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(BOOT_BTN, INPUT_PULLUP);

#ifdef DISPLAY_SPI
  // SPI is write-only: there is no ACK, so begin() succeeding proves the driver
  // initialised, NOT that a display is attached or wired correctly. Only seeing
  // pixels confirms that.
  displayOk = display.begin(0, true);
  Serial.printf("[oled] SPI mode: MOSI/SDA=GPIO%d CLK/SCK=GPIO%d DC=GPIO%d RST=GPIO%d CS=GPIO%d\n",
                OLED_MOSI, OLED_CLK, OLED_DC, OLED_RST, OLED_CS);
  if (!displayOk) Serial.println("[oled] SPI init failed - continuing headless");
#else
  Wire.begin(SDA_PIN, SCL_PIN);
  displayOk = display.begin(OLED_ADDR, true);
  if (!displayOk) {
    // Do NOT halt. A loose I2C wire must not cost you the network endpoint,
    // or the failure becomes diagnosable only over a USB cable.
    Serial.printf("[oled] not found at 0x%02X - continuing headless\n", OLED_ADDR);
    Serial.printf("[oled] check wiring: SDA=GPIO%d, SCL=GPIO%d, VCC=3.3V\n", SDA_PIN, SCL_PIN);
    diagnoseI2C();
  }
#endif

  if (displayOk) {
    display.clearDisplay();
    display.display();
  }

  // Show what is actually flashed, so a stale board is obvious at a glance.
  Serial.printf("[boot] Clauled %s\n", CLAULED_VERSION);
  oledStatus("Clauled", "v" CLAULED_VERSION);
  if (displayOk) delay(1200);

  // Two attempts, then move on. If the network is unavailable the display is
  // still the point of the device, and loop() keeps trying in the background.
  if (connectWiFi(2)) {
    startMDNS();
    startServer();
    serverStarted = true;
    netUp = true;
  } else {
    Serial.println("[wifi] continuing without network - retrying every 30s");
  }
  lastWifiTry = millis();

  drawScreen();
  lastCycle = millis();
  lastDraw  = millis();
}

void loop() {
  server.handleClient();

  // Non-blocking WiFi management. Fire an attempt periodically and let the
  // driver work in the background - never stall the render loop waiting on it.
  if (WiFi.status() != WL_CONNECTED) {
    if (netUp) {
      Serial.println("[wifi] lost connection");
      netUp = false;
      deviceIP = "";
    }
    if (millis() - lastWifiTry >= WIFI_RETRY_MS) {
      Serial.println("[wifi] background retry");
      WiFi.disconnect(false, false);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      lastWifiTry = millis();
    }
  } else if (!netUp) {
    deviceIP = WiFi.localIP().toString();
    Serial.printf("[wifi] connected, IP=%s\n", deviceIP.c_str());
    startMDNS();                                   // safe to re-run
    if (!serverStarted) { startServer(); serverStarted = true; }
    netUp = true;
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
