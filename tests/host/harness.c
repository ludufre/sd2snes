/* Host harness for the IPS/BPS patch engine (src/patch.c, compiled UNMODIFIED
 * against the shim headers).
 *
 * Stages the patch path exactly like the firmware does (slot 1 of the SRAM
 * IPS list), loads a source ROM at SRAM_ROM_ADDR, runs the real entry point
 * and reports via exit code:
 *
 *   0   patcher succeeded (and the output matched <expected>, when given)
 *   1   patcher failed cleanly (returned 0)
 *   2   patcher "succeeded" but the output does not match <expected>
 *   124 HANG: the watchdog fired (the forbidden MCU failure mode)
 *   125 OVERFLOW: bytes outside the legal window were written
 *
 * usage: harness apply <patch> <source_rom> [expected_target]
 *        harness probe <patch> <source_rom>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "host.h"
#include "patch.h"

#define CANARY 0xA5
#define WATCHDOG_SECS 10

extern uint32_t patch_apply(uint32_t sram_addr, uint8_t index,
                            uint32_t rom_base_addr, uint32_t original_rom_size,
                            uint32_t rom_header_size);

static void on_alarm(int sig) {
  (void)sig;
  /* async-signal-safe exit: the patcher is wedged in an infinite loop */
  _exit(124);
}

static uint32_t load_file(const char *path, uint32_t addr) {
  FILE *fp = fopen(path, "rb");
  if (!fp) { fprintf(stderr, "cannot open %s\n", path); exit(99); }
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (fread(host_sdram + addr, 1, (size_t)sz, fp) != (size_t)sz) exit(99);
  fclose(fp);
  return (uint32_t)sz;
}

/* The patcher may legally touch [SRAM_ROM_ADDR, SRAM_SAVE_ADDR) (ROM image,
 * BPS source backup, probe scratch) and the IPS list page we staged. Anything
 * else it writes is region corruption (menu image, SaveRAM, cfg staging). */
static int region_is_legal(uint32_t a) {
  if (a < SRAM_SAVE_ADDR) return 1;
  if (a >= SRAM_IPS_LIST_ADDR && a < SRAM_IPS_LIST_ADDR + 0x1000) return 1;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s apply|probe <patch> <source_rom> [expected_target]\n", argv[0]);
    return 99;
  }
  const char *mode = argv[1], *patch = argv[2], *src = argv[3];
  const char *expected = (argc > 4) ? argv[4] : NULL;

  host_sdram_init();

  uint32_t romsize = load_file(src, SRAM_ROM_ADDR);

  /* stage the patch path in slot 1, like ips_find_patches would */
  size_t plen = strlen(patch) + 1;
  if (plen > IPS_PATH_LEN) { fprintf(stderr, "patch path too long\n"); return 99; }
  memcpy(host_sdram + SRAM_IPS_LIST_ADDR + 512, patch, plen);

  /* canary-fill every region the patcher must not touch */
  for (uint32_t a = SRAM_SAVE_ADDR; a < 0x1000000u; a++)
    if (!region_is_legal(a)) host_sdram[a] = CANARY;

  signal(SIGALRM, on_alarm);
  alarm(WATCHDOG_SECS);

  uint32_t ret;
  if (strcmp(mode, "probe") == 0) {
    uint32_t scratch = 0;
    ret = bps_probe_header(SRAM_IPS_LIST_ADDR, 1, SRAM_ROM_ADDR, romsize,
                           PATCH_PROBE_HEADER_LIMIT, &scratch);
  } else if (strcmp(mode, "copier") == 0) {
    /* Copier mode: decode emits descriptors for SourceCopy/TargetCopy and only
       the source backup + TargetRead literals are written inline; replaying the
       descriptors over the fake SDRAM (like the live menu running the FPGA
       copier) must finalize a byte-identical image -> the [expected] check below
       proves the copier decomposition equals the byte-by-byte apply. */
    ret = patch_apply_copier(SRAM_IPS_LIST_ADDR, 1, SRAM_ROM_ADDR, romsize, 0);
    if (ret) host_copier_replay();
  } else {
    ret = patch_apply(SRAM_IPS_LIST_ADDR, 1, SRAM_ROM_ADDR, romsize, 0);
  }
  alarm(0);

  for (uint32_t a = SRAM_SAVE_ADDR; a < 0x1000000u; a++) {
    if (!region_is_legal(a) && host_sdram[a] != CANARY) {
      fprintf(stderr, "OVERFLOW: wrote outside the ROM window at 0x%06x\n", a);
      return 125;
    }
  }

  if (ret == 0) return 1;  /* clean failure */

  if (expected) {
    FILE *fp = fopen(expected, "rb");
    if (!fp) { fprintf(stderr, "cannot open %s\n", expected); return 99; }
    fseek(fp, 0, SEEK_END);
    long esz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *want = malloc((size_t)esz);
    if (fread(want, 1, (size_t)esz, fp) != (size_t)esz) return 99;
    fclose(fp);
    if (memcmp(host_sdram + SRAM_ROM_ADDR, want, (size_t)esz) != 0) {
      for (long i = 0; i < esz; i++)
        if (host_sdram[SRAM_ROM_ADDR + i] != want[i]) {
          fprintf(stderr, "MISMATCH at +0x%lx: got %02x want %02x\n",
                  i, host_sdram[SRAM_ROM_ADDR + i], want[i]);
          break;
        }
      return 2;
    }
    free(want);
  }
  printf("ok: ret=0x%x\n", ret);
  return 0;
}
