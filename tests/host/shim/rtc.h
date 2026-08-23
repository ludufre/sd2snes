/* Host shim for the platform rtc.h (src/lpc175x/rtc.h): cfg.c needs only the
 * two S-RTC <-> packed-BCD converters, which run_cfg.sh extracts from the real
 * rtc.c at build time. */
#ifndef HOST_RTC_H
#define HOST_RTC_H

#include <stdint.h>

void bcdtime2srtctime(uint64_t bcdtime, uint8_t *srtctime);
uint64_t srtctime2bcdtime(uint8_t *srtctime);

#endif
