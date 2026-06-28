// web.h - HTTP file manager + WiFi config, same REST surface as the ESP32 build.
#pragma once
void web_init(void);            // register routes once at boot (does NOT listen yet)
void web_set_enabled(bool on);  // begin()/stop() the listener, tied to the WiFi radio
void web_loop(void);            // call from loop(): server.handleClient() when up
