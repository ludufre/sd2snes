// web.h - HTTP file manager + WiFi config, same REST surface as the ESP32 build.
#pragma once
void web_start(void);
void web_loop(void);   // call from loop(): server.handleClient()
