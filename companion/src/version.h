// version.h - cosmetic display version only.
//
// Shown on the SNES "System Information" screen as: Companion: <str> (CHIP).
// The SD self-update does NOT use this. It flashes whenever /sd2snes/espXX.bin
// differs from what was last applied (a CRC32 of the image stored in the .bin
// trailer, compared against EEPROM) - just like the sd2snes MCU bootloader
// re-flashes firmware.imX when the content differs. So you never need to bump a
// number for an update; this string is only what the menu displays.
#pragma once
#define FW_VERSION_STR "1.0.0"
