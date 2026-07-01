/* Host-side implementations behind the shim headers: a 16 MB SDRAM array,
 * the FPGA SPI-window state machine patch.c's psram_* helpers drive, the
 * fileops globals over stdio, and CRC-32. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "fpga_spi.h"
#include "cfg.h"
#include "crc32.h"
#include "patch_copier.h"
#include "host.h"

cfg_t CFG = { 0 };

/* ---- 16 MB SDRAM, 24-bit address wrap like the real window ------------- */
#define SDRAM_SIZE 0x1000000u
#define SDRAM_MASK 0x0FFFFFFu

uint8_t *host_sdram;

void host_sdram_init(void) {
  host_sdram = calloc(SDRAM_SIZE, 1);
  if (!host_sdram) { fprintf(stderr, "calloc sdram failed\n"); exit(99); }
}

/* ---- FPGA SPI-window state machine -------------------------------------
 * SELECT resets to the command phase; the first TX byte is a command:
 *   SETADDR|TGT_MEM -> next 3 TX bytes are the 24-bit cursor (hi..lo)
 *   0x98            -> write mode: every TX byte stores at cursor++
 *   0x88            -> read mode:  every RX byte loads from cursor++       */
enum { PH_CMD, PH_A2, PH_A1, PH_A0, PH_WRITE, PH_READ };
static int      fpga_phase = PH_CMD;
static uint32_t fpga_cur;

void host_fpga_select(void)   { fpga_phase = PH_CMD; }
void host_fpga_deselect(void) { }

void host_fpga_tx(uint8_t b) {
  switch (fpga_phase) {
    case PH_CMD:
      if (b == (FPGA_CMD_SETADDR | FPGA_TGT_MEM)) fpga_phase = PH_A2;
      else if (b == 0x98) fpga_phase = PH_WRITE;
      else if (b == 0x88) fpga_phase = PH_READ;
      else { fprintf(stderr, "fpga shim: unknown cmd 0x%02x\n", b); exit(99); }
      break;
    case PH_A2: fpga_cur = (uint32_t)b << 16;  fpga_phase = PH_A1; break;
    case PH_A1: fpga_cur |= (uint32_t)b << 8;  fpga_phase = PH_A0; break;
    case PH_A0: fpga_cur |= b; fpga_cur &= SDRAM_MASK; fpga_phase = PH_CMD; break;
    case PH_WRITE:
      host_sdram[fpga_cur] = b;
      fpga_cur = (fpga_cur + 1) & SDRAM_MASK;
      break;
    default:
      fprintf(stderr, "fpga shim: TX in read mode\n"); exit(99);
  }
}

uint8_t host_fpga_rx(void) {
  if (fpga_phase != PH_READ) { fprintf(stderr, "fpga shim: RX outside read mode\n"); exit(99); }
  uint8_t b = host_sdram[fpga_cur];
  fpga_cur = (fpga_cur + 1) & SDRAM_MASK;
  return b;
}

/* ---- sram_* accessors (non-windowed firmware path) ---------------------- */
void sram_writebyte(uint8_t val, uint32_t addr) { host_sdram[addr & SDRAM_MASK] = val; }
uint8_t sram_readbyte(uint32_t addr) { return host_sdram[addr & SDRAM_MASK]; }
void sram_writeblock(const void *buf, uint32_t addr, uint16_t size) {
  for (uint16_t i = 0; i < size; i++)
    host_sdram[(addr + i) & SDRAM_MASK] = ((const uint8_t *)buf)[i];
}
void sram_readblock(void *buf, uint32_t addr, uint16_t size) {
  for (uint16_t i = 0; i < size; i++)
    ((uint8_t *)buf)[i] = host_sdram[(addr + i) & SDRAM_MASK];
}

/* ---- fileops/FatFs over stdio ------------------------------------------- */
BYTE    file_buf[512];
FIL     file_handle;
FRESULT file_res;
uint8_t file_lfn[258];

void file_open(const uint8_t *filename, BYTE flags) {
  (void)flags;
  memset(&file_handle, 0, sizeof(file_handle));
  file_handle.fp = fopen((const char *)filename, "rb");
  if (!file_handle.fp) { file_res = FR_NO_FILE; return; }
  fseek(file_handle.fp, 0, SEEK_END);
  file_handle.fsize = (DWORD)ftell(file_handle.fp);
  fseek(file_handle.fp, 0, SEEK_SET);
  file_handle.fptr = 0;
  file_res = FR_OK;
}

void file_close(void) {
  if (file_handle.fp) { fclose(file_handle.fp); file_handle.fp = NULL; }
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  if (!fp->fp) { *br = 0; return FR_INT_ERR; }
  *br = (UINT)fread(buff, 1, btr, fp->fp);
  fp->fptr += *br;
  return FR_OK;
}

FRESULT f_lseek(FIL *fp, DWORD ofs) {
  if (!fp->fp) return FR_INT_ERR;
  if (ofs > fp->fsize) ofs = fp->fsize;  /* FatFs clamps in read-only mode */
  fseek(fp->fp, (long)ofs, SEEK_SET);
  fp->fptr = ofs;
  return FR_OK;
}

FRESULT f_opendir(DIR *dp, const TCHAR *path) { (void)dp; (void)path; return FR_NO_PATH; }
FRESULT f_readdir(DIR *dp, FILINFO *fno) { (void)dp; fno->fname[0] = 0; return FR_OK; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }

/* ---- BPS copier model: collect descriptors, replay like the FPGA copier ----
 * bps_apply(use_copier=1) calls patch_copier_emit() instead of byte-moving the
 * SourceCopy/TargetCopy bulk.  The firmware packs these for the live menu to run;
 * here we record them and host_copier_replay() applies a forward element-by-
 * element copy over host_sdram -- the exact FPGA copier semantics, so overlapping
 * src<dst reproduces RLE inflate.  Replaying AFTER bps_apply returns (TargetRead
 * literals + source backup already written inline) models the firmware ordering:
 * descriptors read pristine source / already-finalized output. */
/* Approach B+: patch_copier_emit ACCUMULATES SourceCopy/TargetCopy as descriptors
   (the firmware batches them into a PSRAM list); patch_copier_finish replays them in
   order -- a forward element-by-element copy is exactly the FPGA copier (overlapping
   src<dst reproduces RLE inflate).  The source backup runs immediately via
   patch_copier_op_now (must read pristine source before run_actions).  Deferred
   replay is byte-identical because the inline TargetReads are already applied and
   the copier ops replay in action order.  host_copier_replay() is a no-op (finish
   already did the work; kept so the CLI/harness call sites stay unchanged). */
struct hc_op { uint32_t src, dst, len; };
static struct hc_op *hc_ops;
static uint32_t hc_n, hc_cap;

void patch_copier_reset(uint32_t list_base) { (void)list_base; hc_n = 0; }
uint32_t patch_copier_count(void) { return hc_n; }

static void hc_copy(uint32_t src, uint32_t dst, uint32_t len) {
  for (uint32_t j = 0; j < len; j++)
    host_sdram[(dst + j) & SDRAM_MASK] = host_sdram[(src + j) & SDRAM_MASK];
}

int patch_copier_op_now(uint32_t src, uint32_t dst, uint32_t len) {
  hc_copy(src, dst, len);   /* immediate (source backup) */
  return 0;
}

int patch_copier_emit(uint32_t src, uint32_t dst, uint32_t len) {
  if (hc_n >= hc_cap) {
    hc_cap = hc_cap ? hc_cap * 2 : 1024;
    hc_ops = realloc(hc_ops, hc_cap * sizeof(*hc_ops));
    if (!hc_ops) { fprintf(stderr, "hc_ops realloc failed\n"); exit(99); }
  }
  hc_ops[hc_n].src = src; hc_ops[hc_n].dst = dst; hc_ops[hc_n].len = len;
  hc_n++;
  return 0;
}

int patch_copier_finish(void) {
  for (uint32_t i = 0; i < hc_n; i++)
    hc_copy(hc_ops[i].src, hc_ops[i].dst, hc_ops[i].len);
  return 0;
}

void host_copier_replay(void) { /* no-op: patch_copier_finish already replayed */ }

/* ---- CRC-32 (reflected, poly 0xEDB88320 -- same as BPS/zlib) ------------ */
uint32_t crc32_init(void) { return 0xFFFFFFFFu; }
uint32_t crc32_update(uint32_t crc, uint8_t b) {
  crc ^= b;
  for (int i = 0; i < 8; i++)
    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  return crc;
}
uint32_t crc32_finalize(uint32_t crc) { return ~crc; }
