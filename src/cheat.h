#ifndef CHEAT_H
#define CHEAT_H

/* sd2snes cheat capabilities:
 *  -  6 ROM patches
 *  - 20 WRAM patches
 *  - in-game button shortcuts to en/disable cheats
 */

/* menu cheat structure:
 *  I. 1 byte: number of following cheat records
 * II. n cheat records
 */

/* cheat record structure:
 *  1 byte : flags (bit 7: cheat enabled; bit 6-0: reserved)
 * 40 bytes: cheat description
 *  1 byte : number of patches for this cheat
 *  N TIMES:
 *     3 bytes: cheat address + bank
 *     1 byte : patch value
 */

#include CONFIG_MCU_H

#define CHEAT_BASEDIR   ("/sd2snes/cheats/")

#define CHEAT_FLAG_ENABLE (0x80)
#define CHEAT_NUM_CODES_PER_CHEAT (40)
/* WRAM cheats are emitted as a 6-byte LDA/STA/RTS chain starting at
   SNESCMD_WRAM_CHEATS; the chain must stay below the next snescmd vector
   (SNESCMD_NMI_RESET). 20 matches the documented capability with margin. */
#define CHEAT_WRAM_MAX (20)
/* Records live in the $D00000 PSRAM bank group D0..D3 (512 * 512 bytes). */
#define CHEAT_RECORD_MAX (512)

/* PSRAM-patched ROM cheats (no FPGA comparators): per-record spare tail.
   Record slot layout is flags(1)+desc(254)+numpatches(1)+patches(40*4)=416
   bytes; the remaining 96 bytes of the 512-byte slot store, per code, the
   original ROM byte and an "applied to the image" flag so a cheat can be
   toggled off by restoring the byte. */
#define CHEAT_REC_ORIG_OFS    (416)
#define CHEAT_REC_APPLIED_OFS (456)

/* Apply/restore ROM codes directly into the loaded ROM image in PSRAM.
   Active only on builds/cores without the FPGA cheat comparators (mk2 SA-1);
   no-op otherwise. Idempotent; call sites: deassert_reset() (after every
   image mutation: stream, IPS/BPS, recore) and the in-game reprogram. */
void cheat_rom_psram_apply(void);
uint8_t cheat_rom_psram_mode(void);

/* In-game cheat overlay names: a SLIDING WINDOW of CHEAT_NAME_INGAME_MAX descriptions in the
 * SNES-visible BSRAM window (SRAM_CHEAT_NAMES_ADDR, $FF8000). The canonical PSRAM records at
 * $D00000 hold the full descriptions, but during a game that bank IS the game's own ROM, so the
 * in-game overlay cannot reach them — it reads these BSRAM copies. The base-0 window (first 64)
 * is staged at game load (zero extra cost); when the user scrolls past it the overlay requests a
 * re-based window via CMD_CHEAT_NAMES_WINDOW and the MCU restages 64 names from the records into
 * $FF8000. So the overlay lists ALL cheats (up to CHEAT_RECORD_MAX) without growing the game-load
 * staging. 64*64 = 4 KB at $FF8000..$FF8FFF. LEN is a power of two so the window offset
 * ((idx - base)*LEN) is a simple shift. */
#define CHEAT_NAME_INGAME_MAX (64)
#define CHEAT_NAME_INGAME_LEN (64)

/* Re-stage the 64-name window for absolute cheat indices [base, base+64) from the canonical
 * $D00000 records into SRAM_CHEAT_NAMES_ADDR, then publish `base` to SRAM_CHEAT_WIN_BASE_ADDR.
 * Served on CMD_CHEAT_NAMES_WINDOW while the SNES is frozen in the overlay. Bounded (64 reads,
 * no SD, no YAML); slots past the cheat count are staged empty. */
void cheat_stage_names_window(int base);

typedef union _cheat_patch_record {
  struct __attribute__ ((__packed__)) _patch_fields {
    uint8_t  patchvalue;
    uint16_t patchaddr;
    uint8_t  patchbank;
  } fields;
  uint32_t code;
} cheat_patch_record_t;

typedef struct __attribute__ ((__packed__)) _cheat_record {
  uint8_t flags;
  char description[254];
  uint8_t numpatches;
  cheat_patch_record_t patches[40];
} cheat_record_t;

/* deploy all cheats to SNES code / FPGA */
void cheat_program(void);

/* deploy a single cheat record */
void cheat_program_single(cheat_patch_record_t *cheat);

/* deploy ROM cheat to FPGA */
void cheat_program_rom_cheat(int index, cheat_patch_record_t *cheat);

/* deploy WRAM cheat to SNES code */
void cheat_program_ram_cheat(int index, cheat_patch_record_t *cheat);

/* load CHT file to RAM */
void cheat_load_to_menu(int index, cheat_record_t *cheat);
void cheat_save_from_menu(int index, cheat_record_t *cheat);

/* enable/disable ROM cheats + hooks */
void cheat_enable(int enable);
void cheat_nmi_enable(int enable);
void cheat_irq_enable(int enable);
void cheat_holdoff_enable(int enable);
void cheat_buttons_enable(int enable);
void cheat_wram_present(int enable);

/* read cheats from YAML file and convert to SNES structure */
void cheat_yaml_load(uint8_t *romfilename);
/* save SNES structure as YAML file */
void cheat_yaml_save(uint8_t *romfilename);

/* toggle bit 7 of the flag byte for a cheat record (CMD_TOGGLE_CHT) */
void cheat_toggle_flag(int index);

/* in-game live re-program (CMD_CHEAT_REPROGRAM): reconcile the BSRAM flag
   mirror into the canonical PSRAM records and re-deploy all cheats */
void cheat_reprogram_from_mirror(void);

/* convert cheat code in string format to binary */
uint32_t cheat_str2bin(char *string);

/* convert between raw/PAR and GG codes */
uint32_t cheat_gg2raw(uint32_t code);
uint32_t cheat_raw2gg(uint32_t code);

#endif
