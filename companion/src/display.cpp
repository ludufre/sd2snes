// display.cpp - see display.h. ST7789 1.54" (240x240) status panel.
//
// One driver, three wirings (platform.h): ESP32/ESP32-C3 use hardware SPI; the
// ESP8266 uses software SPI because hardware HSPI (MOSI=GPIO13 / SS=GPIO15)
// collides with the swapped UART0 MCU link. The panel only ever draws short text
// rows (cleared with a small fillRect, never a full-screen fill on a repaint), so
// even the bit-banged ESP8266 path stays cheap enough not to stall the link/WDT.
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "platform.h"
#include "net.h"
#include "version.h"

// Most 1.54" 240x240 ST7789 modules want inversion on; flip to 0 if colours look
// negated on a given panel. Column/row offsets are the library defaults (0,0).
#ifndef TFT_INVERT
  #define TFT_INVERT 1
#endif

#define REFRESH_MS 750          // min gap between status checks/repaints

#if TFT_HWSPI
  #include <SPI.h>
  static Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
#else
  // software-SPI constructor: (cs, dc, mosi, sclk, rst); CS is tied to GND (-1)
  static Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
#endif

static bool          s_ok = false;     // panel initialised
static unsigned long s_next = 0;
static bool          s_drawn = false;   // a status frame has been painted at least once
static net_status    s_last;            // last painted state (for redraw-on-change)

// clear one text row then print it (size 1 = 6x8 px, size 2 = 12x16 px glyphs)
static void row(int16_t y, uint8_t size, uint16_t color, const char *s) {
    int16_t h = (int16_t)(8 * size) + 2;
    tft.fillRect(0, y, 240, h, ST77XX_BLACK);
    tft.setTextSize(size);
    tft.setTextColor(color, ST77XX_BLACK);
    tft.setCursor(4, y);
    tft.print(s);
}

void display_init(void) {
#if TFT_HWSPI
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);   // sck, miso(unused), mosi, ss(lib-driven)
#endif
#if (TFT_BL >= 0)
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);              // backlight on (BL tied to 3V3 when < 0)
#endif
    tft.init(240, 240);
    tft.setRotation(0);
    tft.invertDisplay(TFT_INVERT);
    tft.fillScreen(ST77XX_BLACK);            // one-time clear (the only full fill)
    s_ok = true;

    row(8,  2, ST77XX_CYAN,  "sd2snes");
    row(32, 2, ST77XX_CYAN,  "companion");
    row(64, 1, ST77XX_WHITE, FW_VERSION_STR " (" CHIP_NAME ")");
    row(84, 1, ST77XX_YELLOW, "starting...");
    s_drawn = false;
}

void display_loop(void) {
    if (!s_ok) return;
    if ((int32_t)(millis() - s_next) < 0) return;
    s_next = millis() + REFRESH_MS;

    net_status st;
    net_get_status(&st);

    // repaint only when something the panel shows actually changed
    if (s_drawn &&
        st.connected == s_last.connected &&
        st.rssi == s_last.rssi &&
        strncmp(st.ssid, s_last.ssid, sizeof(st.ssid)) == 0 &&
        strncmp(st.ip,   s_last.ip,   sizeof(st.ip))   == 0)
        return;
    s_last = st;
    s_drawn = true;

    char line[48];
    row(64, 1, ST77XX_WHITE, FW_VERSION_STR " (" CHIP_NAME ")");

    if (st.connected) {
        snprintf(line, sizeof(line), "WiFi: %s", st.ssid);
        row(96,  2, ST77XX_GREEN, line);
        snprintf(line, sizeof(line), "IP: %s", st.ip);
        row(128, 2, ST77XX_WHITE, line);
        snprintf(line, sizeof(line), "RSSI: %d dBm", (int)st.rssi);
        row(160, 1, ST77XX_WHITE, line);
    } else {
        snprintf(line, sizeof(line), "AP: %s", net_ap_ssid());
        row(96,  2, ST77XX_YELLOW, line);
        row(128, 2, ST77XX_WHITE, "IP: 192.168.4.1");
        row(160, 1, ST77XX_WHITE, "waiting for WiFi...");
    }
}
