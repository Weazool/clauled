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
// Every one of these is a BACKSTOP, not the primary mechanism. The host
// explicitly clears a spinner (Stop sends busy:"") and supersedes a banner,
// so in normal operation nothing here should ever be what ends a state -
// these only decide how long a state survives if the push that was supposed
// to end it never arrived.
#define BUSY_TTL_S           90   // spinner self-expires this long after the last busy push.
                                   // Was 180. A false "working" on an idle session is more
                                   // misleading than a spinner that stops early during a long
                                   // stretch with no tool calls, and 3 minutes of it was a lot.
#define EVENT_TTL_S         300   // "Your turn" banner self-expires after this. Normally cleared
                                   // far sooner - the next prompt's busy push supersedes it.
#define STALE_AFTER_S       300   // no push for a session this long -> show it as quiet
#define SESSION_GONE_S      900   // no push for a session this long -> drop it from the roster
#define UNCONFIRMED_GONE_S  120   // a slot that has NEVER shown a title or real context (see
                                   // Session::everPopulated) -> drop it this much sooner. Guards
                                   // against a stray or malformed push - a test script, a future
                                   // globals-only push, anything with no real session behind it -
                                   // sitting in the roster for the full SESSION_GONE_S with
                                   // nothing in it.
#define ROTATE_INTERVAL_S     6   // multiple active sessions -> cycle through them this often
                                   // (also the 5h/1w quota alternation rate - same clock)

// Quiet hours are a HOST decision - the device has no clock and never will
// (that is why NTP was removed entirely; see CHANGELOG v2.0.0). The host
// sends "quiet": true/false on every push, computed from its own local time.
// This is only the idle threshold this device waits for once quiet hours are
// in effect before actually powering the panel off.
#define QUIET_IDLE_S        900   // no push from ANY session this long during quiet hours -> display off
