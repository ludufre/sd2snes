#ifndef _LANG_H
#define _LANG_H

#include <stdint.h>

typedef enum { LANG_EN = 0, LANG_PTBR = 1, LANG_ES = 2, LANG_DE = 3, NUM_LANG } lang_t;

/* sysinfo localized message IDs */
enum {
  SI_BUSY_DISK = 0,       /* "Calculating disk space..."   (fixed 40-char block) */
  SI_FW_VERSION,          /* "    Firmware version: %s"                          */
  SI_SD_REMOVED,          /* "    *** SD Card removed/USB busy ***    "          */
  SI_SD_MAKER,            /* "SD Maker/OEM:    0x%02x, \"%c%c\""                 */
  SI_SD_PRODUCT,          /* "SD Product Name: \"%c%c%c%c%c\", Rev. %d.%d"       */
  SI_SD_SERIAL,           /* "SD Serial No.:   %02x%02x%02x%02x, Mfd. %d/%02d"   */
  SI_SD_ACC_TIME,         /* "SD acc. time: %ld.%03ld / %ld.%03ld ms avg/max"   */
  SI_SD_ACC_MEASURING,    /* "SD acc. time: measuring..."                       */
  SI_CARD_USAGE,          /* "Card usage: %ldMB / %ldMB"                        */
  SI_CIC_STATE,           /* "CIC state: %s"                                    */
  SI_SNES_CLK_MEASURING,  /* "SNES master clock: measuring..."                  */
  SI_SNES_CLK,            /* "SNES master clock: %ldHz"                         */
  SI_COUNT
};

/* SGB BIOS state words (indexed: 0=missing 1=mismatch 2=ok 3=checking) */
enum {
  SGB_W_MISSING = 0,
  SGB_W_MISMATCH,
  SGB_W_OK,
  SGB_W_CHECKING,
  SGB_W_COUNT
};

enum {
  CHEAT_FOR,
  CHEAT_NO_NAME,
  CHEATMENU_COUNT
};

extern const char *const cicstatefriendly_l[NUM_LANG][4];
extern const char *const fresult_friendly_names_l[NUM_LANG][20];
extern const char *const sysinfo_msg[NUM_LANG][SI_COUNT];
extern const char *const sgb_state_l[NUM_LANG][SGB_W_COUNT];
extern const char *const cheatmenu_l[NUM_LANG][CHEATMENU_COUNT];

/* current language, clamped to a valid index */
uint8_t lang_idx(void);

#endif
