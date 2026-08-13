// ============================================================
//  Clauled configuration
//
//  Nothing here is secret, so this file is tracked in git. There is no
//  copy-this-file step and nothing to fill in before flashing - the device
//  holds no credentials of any kind.
//
//  Edit only if your display is wired differently from the defaults.
// ============================================================

#pragma once

// ── Display interface ─────────────────────────────────────────
// Define DISPLAY_SPI for a 7-pin SPI module (GND VCC D0/SCK D1/SDA RES DC CS).
// Comment it out for a 4-pin I2C module (GND VCC SCL SDA).
//
// Safe GPIOs on ESP32-C3: 0, 1, 3, 4, 5, 6, 7, 10.
// Avoid 2 and 8 (boot strapping), 9 (BOOT button), 11-17 (SPI flash),
// and 18/19 (USB - you would lose serial and flashing).
#define DISPLAY_SPI

// SPI wiring (used when DISPLAY_SPI is defined)
#define OLED_MOSI           4      // module pin "SDA" / "D1"
#define OLED_CLK            5      // module pin "SCK" / "D0"
#define OLED_DC             6
#define OLED_RST            7
#define OLED_CS            10

// I2C wiring (used when DISPLAY_SPI is NOT defined)
#define SDA_PIN             4
#define SCL_PIN             5
#define OLED_ADDR           0x3C

// ── Display behaviour ─────────────────────────────────────────
#define CYCLE_TIME          5      // seconds per page; 0 = manual (BOOT button)
#define SHOW_WEEKLY_SONNET  false  // separate Sonnet weekly bucket (Max plans)
#define SHOW_UPTIME         false  // show a device-uptime page
#define STALE_AFTER_S       300    // no push for this long -> mark data stale
