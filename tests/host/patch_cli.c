/* patch_cli.c — apply an IPS/BPS patch to a ROM with the REAL firmware patch
 * engine (src/patch.c, compiled UNMODIFIED against the host shims) and write the
 * resulting ROM, so you can boot it in an emulator.
 *
 * This is the SAME code path the device runs (patch_apply -> ips_apply/bps_apply
 * over the FPGA-window shim into a 16 MB fake SDRAM), so feeding it the same ROM
 * + .ips/.bps files reproduces exactly what the firmware would produce -- if the
 * emulator result differs from the device, the bug is here and is debuggable
 * (built with ASan/UBSan; a hang trips the 10 s watchdog -> exit 124; writes
 * outside the legal ROM window trip the canary -> exit 125).
 *
 * usage: patch-cli [options] <rom> <patch.ips|.bps> <out>
 *   --header N    bytes of copier header to skip when loading <rom>, also passed
 *                 to the patcher as rom_header_size (mirrors load_rom skipping
 *                 romprops.offset and forwarding it).  Default: auto.
 *   --no-header   force header = 0 (apply to the file exactly as-is)
 *   --probe       BPS only: print the embedded target size + scratch base and
 *                 exit without applying/writing (mirrors bps_probe_header)
 *   -q            quiet (only errors)
 *
 * exit: 0 ok · 1 patch failed (returned 0) · 124 hang · 125 OOB write · 99 usage/io
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
#include "patch_copier.h"

#define CANARY 0xA5
#define WATCHDOG_SECS 10

extern uint32_t patch_apply(uint32_t sram_addr, uint8_t index,
                            uint32_t rom_base_addr, uint32_t original_rom_size,
                            uint32_t rom_header_size);

static void on_alarm(int sig) { (void)sig; _exit(124); }

/* legal-to-touch window, identical to the test harness: the ROM image + BPS
 * source backup + probe scratch live below SRAM_SAVE_ADDR; the staged IPS-list
 * page is the only thing the patcher may write up high. */
static int region_is_legal(uint32_t a) {
  if (a < SRAM_SAVE_ADDR) return 1;
  if (a >= SRAM_IPS_LIST_ADDR && a < SRAM_IPS_LIST_ADDR + 0x1000) return 1;
  return 0;
}

static int quiet = 0;
#define LOG(...) do { if (!quiet) fprintf(stderr, __VA_ARGS__); } while (0)

/* (Re)initialize the fake SDRAM for one apply run: fresh 16 MB, ROM image at
 * SRAM_ROM_ADDR, the patch path staged in IPS slot 1, and the no-touch region
 * above SRAM_SAVE_ADDR canary-filled.  --copier runs this twice (reference, then
 * copier) so each run starts from an identical slate. */
static void setup_sdram(const uint8_t *rombuf, uint32_t romsize, const char *patch) {
  host_sdram_init();
  memcpy(host_sdram + SRAM_ROM_ADDR, rombuf, romsize);
  memcpy(host_sdram + SRAM_IPS_LIST_ADDR + 512, patch, strlen(patch) + 1);
  for (uint32_t a = SRAM_SAVE_ADDR; a < 0x1000000u; a++)
    if (!region_is_legal(a)) host_sdram[a] = CANARY;
}

int main(int argc, char **argv) {
  long header_override = -1;   /* -1 = auto */
  int  do_probe = 0;
  int  do_copier = 0;
  const char *rom = NULL, *patch = NULL, *out = NULL;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--header") && i + 1 < argc) header_override = strtol(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--no-header")) header_override = 0;
    else if (!strcmp(argv[i], "--probe")) do_probe = 1;
    else if (!strcmp(argv[i], "--copier")) do_copier = 1;
    else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = 1;
    else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 99; }
    else if (!rom) rom = argv[i];
    else if (!patch) patch = argv[i];
    else if (!out) out = argv[i];
    else { fprintf(stderr, "too many arguments\n"); return 99; }
  }
  if (!rom || !patch || (!out && !do_probe && !do_copier)) {
    fprintf(stderr,
      "usage: %s [--header N|--no-header] [--probe] [--copier] [-q] <rom> <patch.ips|.bps> <out>\n", argv[0]);
    return 99;
  }

  /* ---- load the ROM, skipping any copier header (like load_rom) ---- */
  FILE *fp = fopen(rom, "rb");
  if (!fp) { fprintf(stderr, "cannot open ROM %s\n", rom); return 99; }
  fseek(fp, 0, SEEK_END);
  long fsz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (fsz <= 0) { fprintf(stderr, "empty/invalid ROM %s\n", rom); fclose(fp); return 99; }

  long header = (header_override >= 0) ? header_override
                                       : ((fsz % 1024) == 512 ? 512 : 0);  /* SMC heuristic */
  if (header >= fsz) { fprintf(stderr, "header (%ld) >= ROM size (%ld)\n", header, fsz); fclose(fp); return 99; }
  uint32_t romsize = (uint32_t)(fsz - header);

  uint8_t *rombuf = malloc(romsize);
  if (!rombuf) { fprintf(stderr, "oom\n"); fclose(fp); return 99; }
  fseek(fp, header, SEEK_SET);
  if (fread(rombuf, 1, romsize, fp) != romsize) {
    fprintf(stderr, "short read on ROM\n"); fclose(fp); return 99;
  }
  fclose(fp);

  size_t plen = strlen(patch) + 1;
  if (plen > IPS_PATH_LEN) { fprintf(stderr, "patch path too long (max %d)\n", IPS_PATH_LEN - 1); return 99; }

  const char *ext = strrchr(patch, '.');
  int is_bps = ext && (ext[1] == 'b' || ext[1] == 'B') && (ext[2] == 'p' || ext[2] == 'P');
  LOG("ROM   : %s  (%ld bytes, header %ld -> %u in SDRAM)\n", rom, fsz, header, romsize);
  LOG("patch : %s  (%s)\n", patch, is_bps ? "BPS" : "IPS");

  signal(SIGALRM, on_alarm);

  /* ---- --copier: apply both ways and prove byte-identical output ---------- */
  if (do_copier) {
    /* reference: legacy byte-by-byte apply, fully materialized in SDRAM */
    setup_sdram(rombuf, romsize, patch);
    alarm(WATCHDOG_SECS);
    uint32_t ref = patch_apply(SRAM_IPS_LIST_ADDR, 1, SRAM_ROM_ADDR, romsize, (uint32_t)header);
    alarm(0);
    if (!ref) { fprintf(stderr, "reference apply FAILED (returned 0)\n"); return 1; }
    uint32_t refsz = is_bps ? ref : (ref > romsize ? ref : romsize);
    uint8_t *refbuf = malloc(refsz);
    if (!refbuf) { fprintf(stderr, "oom\n"); return 99; }
    memcpy(refbuf, host_sdram + SRAM_ROM_ADDR, refsz);

    /* copier mode: inline source-backup + TargetRead, descriptors for the rest */
    setup_sdram(rombuf, romsize, patch);
    alarm(WATCHDOG_SECS);
    uint32_t cop = patch_apply_copier(SRAM_IPS_LIST_ADDR, 1, SRAM_ROM_ADDR, romsize, (uint32_t)header);
    alarm(0);
    if (!cop) { fprintf(stderr, "copier apply FAILED (returned 0 / list full)\n"); return 1; }
    host_copier_replay();   /* run the emitted descriptors like the menu copier */

    for (uint32_t a = SRAM_SAVE_ADDR; a < 0x1000000u; a++)
      if (!region_is_legal(a) && host_sdram[a] != CANARY) {
        fprintf(stderr, "OVERFLOW (copier): wrote outside ROM window at 0x%06x\n", a);
        return 125;
      }

    uint32_t copsz = is_bps ? cop : (cop > romsize ? cop : romsize);
    LOG("copier: %u descriptors, target %u bytes\n", patch_copier_count(), copsz);
    if (copsz != refsz) {
      fprintf(stderr, "SIZE MISMATCH: reference=%u copier=%u\n", refsz, copsz);
      return 2;
    }
    uint32_t firstdiff = 0xFFFFFFFFu, ndiff = 0;
    for (uint32_t i = 0; i < refsz; i++)
      if (host_sdram[SRAM_ROM_ADDR + i] != refbuf[i]) {
        if (firstdiff == 0xFFFFFFFFu) firstdiff = i;
        ndiff++;
      }
    if (ndiff) {
      fprintf(stderr,
        "MISMATCH: %u differing bytes, first at 0x%06x (reference %02x, copier %02x)\n",
        ndiff, firstdiff, refbuf[firstdiff], host_sdram[SRAM_ROM_ADDR + firstdiff]);
      return 2;
    }
    LOG("OK    : copier output byte-identical to reference (%u bytes)\n", copsz);
    if (out) {
      FILE *of = fopen(out, "wb");
      if (!of) { fprintf(stderr, "cannot open output %s\n", out); return 99; }
      if (fwrite(host_sdram + SRAM_ROM_ADDR, 1, copsz, of) != copsz) {
        fprintf(stderr, "short write on output\n"); fclose(of); return 99;
      }
      fclose(of);
    }
    return 0;
  }

  setup_sdram(rombuf, romsize, patch);

  alarm(WATCHDOG_SECS);

  uint32_t ret, scratch = 0;
  if (do_probe) {
    ret = bps_probe_header(SRAM_IPS_LIST_ADDR, 1, SRAM_ROM_ADDR, romsize,
                           PATCH_PROBE_HEADER_LIMIT, &scratch);
    alarm(0);
    if (!ret) { fprintf(stderr, "probe FAILED (not a BPS / bad magic)\n"); return 1; }
    printf("probe ok: target_size=%u (0x%x)  scratch_base=0x%06x\n", ret, ret, scratch);
    return 0;
  }

  ret = patch_apply(SRAM_IPS_LIST_ADDR, 1, SRAM_ROM_ADDR, romsize, (uint32_t)header);
  alarm(0);

  /* ---- check the patcher stayed inside the legal window ---- */
  for (uint32_t a = SRAM_SAVE_ADDR; a < 0x1000000u; a++) {
    if (!region_is_legal(a) && host_sdram[a] != CANARY) {
      fprintf(stderr, "OVERFLOW: patcher wrote outside the ROM window at 0x%06x\n", a);
      return 125;
    }
  }

  if (ret == 0) { fprintf(stderr, "patch FAILED (patch_apply returned 0)\n"); return 1; }

  /* Output size: BPS encodes the full target (ret == target_size). IPS patches
   * the ROM in place and returns only the highest offset it touched -- the
   * untouched tail of the original ROM must be preserved, so write the larger of
   * the two. (IPS truncation records are not honored here; rare.) */
  uint32_t out_size = is_bps ? ret : (ret > romsize ? ret : romsize);

  FILE *of = fopen(out, "wb");
  if (!of) { fprintf(stderr, "cannot open output %s\n", out); return 99; }
  if (fwrite(host_sdram + SRAM_ROM_ADDR, 1, out_size, of) != out_size) {
    fprintf(stderr, "short write on output\n"); fclose(of); return 99;
  }
  fclose(of);

  LOG("OK    : wrote %s  (%u bytes", out, out_size);
  if (out_size > romsize) LOG(", ROM grew +%u)\n", out_size - romsize);
  else LOG(")\n");
  return 0;
}
