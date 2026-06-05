// ota.cpp - see ota.h. Self-update from the sd2snes SD card over the UART link.
//
// "Flash if different" (no version numbers), like the sd2snes MCU bootloader.
// The build appends a content trailer to firmware.bin (tools/append_fwtrailer.py):
//   [ ... firmware image ... ][ "SD2ESPFW" (8) | crc32(image) (4) | pad (4) ]
// At boot the ESP reads just the last 16 bytes of /sd2snes/espXX.bin (the MCU GET
// seeks, so no full read), and if that CRC differs from the last one it applied
// (kept in EEPROM) it flashes the image with the Arduino Update library, stores
// the new CRC, and restarts. Same CRC -> nothing happens (no loop).
//
// The Update library validates the image and writes the inactive OTA slot, so a
// corrupt/partial .bin or a power loss leaves the running firmware intact.
#include "platform.h"
#include "proto.h"
#include "link.h"
#include "ota.h"
#include <EEPROM.h>
#include <string.h>

#if defined(ESP32)
  #include <Update.h>
  #define OTA_BIN "/sd2snes/esp32.bin"
#else
  #include <Updater.h>
  #define OTA_BIN "/sd2snes/esp8266.bin"
#endif

#define FW_MAGIC   "SD2ESPFW"   // 8 bytes, must match tools/append_fwtrailer.py
#define FW_TRAILER 16           // magic(8) + crc32(4) + pad(4)
#define EE_SIZE    8
#define EE_CRC_ADDR 0

// Wait (briefly) for the MCU to be servicing the link - at cold boot it may still
// be coming up.
static bool link_ready(void) {
    for (int i = 0; i < 10; i++) {        // up to ~5s
        uint8_t v = 0, l = 0; char nm[8] = {0};
        if (proto_ping(&v, &l, nm, sizeof(nm)) == LOK) return true;
        delay(500);
    }
    return false;
}

// read `len` bytes from the open GET file at `off` into buf; true on full read
static bool get_exact(uint32_t off, uint8_t *buf, uint16_t len) {
    uint16_t r = 0;
    while (r < len) {
        uint8_t st = 0xFF; uint16_t got = 0; int fin = 0;
        if (proto_get_data(off + r, (uint16_t)(len - r), &st, buf + r, &got, &fin) != LOK || st || got == 0)
            return false;
        r += got;
        if (fin) break;
    }
    return r == len;
}

// CRC32 from the SD .bin trailer, or false if absent/invalid. `total` = file size.
static bool sd_bin_crc(uint32_t total, uint32_t *crc) {
    if (total < FW_TRAILER + 4096) return false;
    uint8_t tr[FW_TRAILER];
    if (!get_exact(total - FW_TRAILER, tr, FW_TRAILER)) return false;
    if (memcmp(tr, FW_MAGIC, 8) != 0) return false;
    *crc = (uint32_t)tr[8] | ((uint32_t)tr[9] << 8) |
           ((uint32_t)tr[10] << 16) | ((uint32_t)tr[11] << 24);
    return true;
}

// flash the first `imageSize` bytes of the (already-open) SD .bin (skips the trailer)
static bool ota_apply(uint32_t imageSize) {
    if (!Update.begin(imageSize)) {
        DBG_SERIAL.print(F("OTA: begin failed: ")); Update.printError(DBG_SERIAL);
        return false;
    }
    uint8_t buf[UP_CHUNK]; uint32_t off = 0;
    while (off < imageSize) {
        uint16_t want = (imageSize - off) > UP_CHUNK ? UP_CHUNK : (uint16_t)(imageSize - off);
        uint8_t st = 0xFF; uint16_t got = 0; int fin = 0;
        if (proto_get_data(off, want, &st, buf, &got, &fin) != LOK || st || got == 0) {
            DBG_SERIAL.println(F("OTA: read error")); Update.end(false); return false;
        }
        if (Update.write(buf, got) != got) {
            DBG_SERIAL.print(F("OTA: write error: ")); Update.printError(DBG_SERIAL);
            Update.end(false); return false;
        }
        off += got;
        yield();                       // feed the WDT during the multi-second read
    }
    if (!Update.end(true)) {
        DBG_SERIAL.print(F("OTA: end failed: ")); Update.printError(DBG_SERIAL);
        return false;
    }
    return true;
}

void ota_check_and_apply(void) {
    if (!link_ready()) { DBG_SERIAL.println(F("OTA: MCU link not ready, skipping")); return; }
    uint8_t st = 0xFF; uint32_t total = 0;
    if (proto_get_open(OTA_BIN, &st, &total) != LOK || st) return;   // no .bin on the card
    uint32_t sd_crc = 0;
    if (!sd_bin_crc(total, &sd_crc)) { DBG_SERIAL.println(F("OTA: .bin has no trailer, skipping")); return; }

    EEPROM.begin(EE_SIZE);
    uint32_t applied = 0;
    EEPROM.get(EE_CRC_ADDR, applied);
    DBG_SERIAL.printf("OTA: SD crc=0x%08x, applied=0x%08x\n", sd_crc, applied);
    if (sd_crc == applied) return;     // same firmware as last applied -> nothing to do

    DBG_SERIAL.println(F("OTA: SD firmware differs, updating..."));
    if (ota_apply(total - FW_TRAILER)) {
        EEPROM.put(EE_CRC_ADDR, sd_crc);
        EEPROM.commit();
        DBG_SERIAL.println(F("OTA: done, restarting into the new firmware"));
        delay(300);
        ESP.restart();
    } else {
        DBG_SERIAL.println(F("OTA: failed, keeping the current firmware"));
    }
}
