// wifi_bridge.h - background task that bridges the SNES menu's WiFi requests to
// the ESP WiFi stack over the UART link. The ESP polls the MCU for a pending
// menu request (scan/connect/forget), runs it locally, and pushes status + scan
// results back so the menu can display them. Keeps the MCU a pure server.
#pragma once

void wifi_bridge_start(void);   // call once after proto_init() + wifi_start()
