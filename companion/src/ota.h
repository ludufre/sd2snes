// ota.h - self-update from the sd2snes SD card over the UART link.
#pragma once
void ota_check_and_apply(void);   // call once at boot, right after link_init()
