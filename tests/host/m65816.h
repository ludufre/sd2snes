/* m65816.h -- 65816 interpreter plus enough of the SNES bus to EXECUTE the
 * real NES renderer (misc/nes_snes.bin) on the host.
 *
 * Not an emulator: no cycles, no PPU rendering, no IRQ/NMI, no APU.  What is
 * modelled is what the renderer's CHR-RAM path touches -- WRAM $7E/$7F, the
 * LoROM page in bank $00, the VRAM ports ($2115-$2119), the V counter
 * ($2137/$213D/$213F) and general-purpose DMA ($420B/$43xx).  Anything else
 * ABORTS with address and opcode; ignoring an unknown access would hand back
 * a worthless PASS.  Registers that only shape the SCREEN are mute BY
 * WHITELIST (the list is in reg_write()), never by 'default'.
 *
 * Memory comes up like the device: WRAM filled with $55 (the fork's
 * clear_wram), VRAM POISONED with $A5 -- nes_boot_init is what clears it.
 */
#ifndef M65816_H
#define M65816_H

#include <stddef.h>
#include <stdint.h>

/* --- P register flags --- */
#define M_C 0x01
#define M_Z 0x02
#define M_I 0x04
#define M_D 0x08
#define M_X 0x10
#define M_M 0x20
#define M_V 0x40
#define M_N 0x80

typedef struct {
  uint16_t a, x, y, s, d, pc;
  uint8_t  pbr, dbr, p;
  int      e;             /* emulation (0 = native; the renderer runs native) */
  uint64_t instrs;        /* instructions executed (anti-hang budget) */
} m_cpu_t;

extern m_cpu_t  m_cpu;
extern uint8_t  m_vram[0x10000];
extern uint16_t m_vcounter;      /* V returned by $213D (set by the driver) */
extern int      m_forced_blank;  /* last $2100 bit7 seen */
extern uint64_t m_instr_budget;  /* instruction ceiling per m_call() */

/* Reset the model: WRAM = $55, VRAM = $A5 (poisoned), CGRAM/OAM/registers = 0. */
void m_reset_memory(void);
/* Load the LoROM image (32KB = upper half of bank $00). */
void m_load_rom(const uint8_t *rom, size_t len);

/* HOST access: raw WRAM/ROM, no register decode. */
uint8_t m_peek(uint32_t addr24);
void    m_poke(uint32_t addr24, uint8_t v);
uint16_t m_peek16(uint32_t addr24);
void    m_poke16(uint32_t addr24, uint16_t v);
void    m_poke_block(uint32_t addr24, const void *src, size_t n);

/* Micro-tests of the model itself (index truncation via SEP/PLP/RTI, MVN,
 * VMAIN + mode-1 DMA, m_call through JSL).  Returns the failure count and
 * prints each one.  Leaves the model RESET -- call it before staging. */
int m_selftest(void);

/* Run a routine until its matching RTS/RTL.  `is_long` = called through JSL
 * (3-byte return).  Aborts if m_instr_budget is exceeded. */
void m_call(uint32_t addr24, int is_long);

#endif /* M65816_H */
