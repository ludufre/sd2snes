// display.h - optional 1.54" ST7789 (240x240) status panel on the companion.
//
// Bring-up scope: init the panel and mirror the companion's WiFi status (AP/STA
// SSID + IP) and identity. NOT the cover-art mirror (protocol range 0x30) - that
// is a later phase. Pins/SPI-mode per chip live in platform.h (TFT_* macros).
//
// Both calls are time-bounded and never block the main loop: display_loop()
// throttles and only repaints on a state change, and the panel is text-only (no
// repeated full-screen fill) so the slow ESP8266 software-SPI path can't starve
// the 921600 MCU link.
#pragma once

void display_init(void);   // call once in setup() after net_start()
void display_loop(void);   // call every loop(); self-throttled, redraw-on-change
