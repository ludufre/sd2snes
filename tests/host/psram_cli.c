/* Conformance test for psram_stream_buf()/psram_stream() (src/psram_io.c),
 * compiled against the REAL source.
 *
 * The two helpers are the chokepoint every SD -> PSRAM transfer goes through,
 * and what matters is the EDGES: refuse a zero-sized buffer instead of
 * spinning on it, treat a short read as a failure, publish the FRESULT of the
 * chunk that failed, and call the DAC pump AFTER each chunk lands.  None of
 * that shows in a "the bytes arrived" check, so each case pins the exact call
 * sequence -- every f_read, sram_writeblock and pump, in order -- over a
 * modelled SD file and a guard-filled PSRAM array.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "psram_io.h"

/* ---- event log ---------------------------------------------------------
 * One character per call: R = f_read, W = sram_writeblock, P = pump, each with
 * its length, so a whole run compares against a short string like
 * "R16 W16 P R16 W16 P".  Ordering IS the contract (pump after the write). */
#define LOG_MAX 64
static char logbuf[LOG_MAX * 12];
static size_t loglen;

static void logev(char kind, unsigned n) {
  int w;
  if (loglen + 16 >= sizeof logbuf) return;
  w = (kind == 'P') ? snprintf(logbuf + loglen, sizeof logbuf - loglen, "%sP", loglen ? " " : "")
                    : snprintf(logbuf + loglen, sizeof logbuf - loglen, "%s%c%u", loglen ? " " : "", kind, n);
  if (w > 0) loglen += (size_t)w;
}

/* ---- fake SD file ------------------------------------------------------ */
#define SD_MAX 8192
static uint8_t  sd_data[SD_MAX];
static uint32_t sd_size, sd_pos;
static unsigned sd_reads;
static unsigned sd_fail_call;      /* 1-based read to fail on; 0 = never */
static FRESULT  sd_fail_res;

FIL     file_handle;
FRESULT file_res;
BYTE    file_buf[512];

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  UINT n;
  (void)fp;
  sd_reads++;
  logev('R', btr);
  if (sd_fail_call && sd_reads == sd_fail_call) { *br = 0; return sd_fail_res; }
  n = btr;
  if (sd_pos + n > sd_size) n = (UINT)(sd_size - sd_pos);
  memcpy(buff, sd_data + sd_pos, n);
  sd_pos += n;
  *br = n;
  return FR_OK;
}

/* ---- fake PSRAM -------------------------------------------------------- */
#define PS_SIZE 8192
#define PS_BASE 0x100
#define PS_GUARD 0xA5
static uint8_t psram[PS_SIZE];

void sram_writeblock(const void *buf, uint32_t addr, uint16_t size) {
  logev('W', size);
  if (addr < PS_BASE || (size_t)addr + size > PS_SIZE) {
    fprintf(stderr, "psram_cli: write outside the modelled window (0x%x +%u)\n",
            (unsigned)addr, size);
    return;
  }
  memcpy(psram + addr, buf, size);
}

static unsigned pumps;
static void test_pump(void) { pumps++; logev('P', 0); }

/* ---- harness ----------------------------------------------------------- */
static int fails;

static void reset(uint32_t filesize) {
  uint32_t i;
  for (i = 0; i < filesize && i < SD_MAX; i++) sd_data[i] = (uint8_t)(i * 7 + 3);
  sd_size = filesize;
  sd_pos = 0;
  sd_reads = 0;
  sd_fail_call = 0;
  sd_fail_res = FR_OK;
  pumps = 0;
  loglen = 0;
  logbuf[0] = 0;
  memset(psram, PS_GUARD, sizeof psram);
  file_res = FR_INVALID_PARAMETER;   /* a value the code under test never sets */
  memset(&file_handle, 0, sizeof file_handle);
}

static void check(const char *name, int cond, const char *what) {
  if (cond) return;
  printf("FAIL  %s: %s\n", name, what);
  fails++;
}

static void check_eq(const char *name, const char *what, long got, long want) {
  if (got == want) return;
  printf("FAIL  %s: %s = %ld, want %ld\n", name, what, got, want);
  fails++;
}

static void check_log(const char *name, const char *want) {
  if (!strcmp(logbuf, want)) return;
  printf("FAIL  %s: call sequence\n        got  \"%s\"\n        want \"%s\"\n",
         name, logbuf, want);
  fails++;
}

/* The first `n` bytes at PS_BASE must be the file, and NOTHING outside
   [PS_BASE, PS_BASE+n) may have moved off the guard byte. */
static void check_psram(const char *name, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; i++)
    if (psram[PS_BASE + i] != sd_data[i]) {
      printf("FAIL  %s: psram[+%u] = %02x, want %02x\n",
             name, (unsigned)i, psram[PS_BASE + i], sd_data[i]);
      fails++;
      return;
    }
  for (i = 0; i < PS_SIZE; i++) {
    if (i >= PS_BASE && i < PS_BASE + n) continue;
    if (psram[i] != PS_GUARD) {
      printf("FAIL  %s: wrote outside the requested window at 0x%x\n", name, (unsigned)i);
      fails++;
      return;
    }
  }
}

/* The check_* helpers report and COUNT, they do not stop the case, so PASS is
   printed only when nothing has failed since the previous verdict.  Every case
   below ends in exactly one pass() call. */
static int last_verdict_fails;

static void pass(const char *name) {
  if (fails == last_verdict_fails) printf("PASS  %s\n", name);
  last_verdict_fails = fails;
}

/* The dangerous failure does not return: drop the bufsz guard and the loop asks
   for 0 bytes forever, so a wedge must be a red exit. */
static void on_alarm(int sig) { (void)sig; _exit(124); }

int main(void) {
  uint8_t buf[64];
  int r;

  signal(SIGALRM, on_alarm);
  alarm(10);

  /* A zero-sized buffer can never advance the copy: looping on a want of 0
     would be an MCU that never returns. */
  reset(256);
  r = psram_stream_buf(&file_handle, PS_BASE, 100, buf, 0, test_pump);
  check_eq("bufsz-zero", "return", r, 0);
  check_eq("bufsz-zero", "f_read calls", sd_reads, 0);
  check_eq("bufsz-zero", "pump calls", pumps, 0);
  check_log("bufsz-zero", "");
  check_psram("bufsz-zero", 0);
  pass("bufsz-zero");

  /* Nothing to copy is success, and it must not cost a read. */
  reset(256);
  r = psram_stream_buf(&file_handle, PS_BASE, 0, buf, sizeof buf, test_pump);
  check_eq("size-zero", "return", r, 1);
  check_eq("size-zero", "f_read calls", sd_reads, 0);
  check_eq("size-zero", "pump calls", pumps, 0);
  check_log("size-zero", "");
  check_psram("size-zero", 0);
  pass("size-zero");

  /* Exact multiple of the buffer: three full chunks, pump after each WRITE. */
  reset(256);
  r = psram_stream_buf(&file_handle, PS_BASE, 48, buf, 16, test_pump);
  check_eq("exact-multiple", "return", r, 1);
  check_eq("exact-multiple", "pump calls", pumps, 3);
  check_log("exact-multiple", "R16 W16 P R16 W16 P R16 W16 P");
  check_psram("exact-multiple", 48);
  pass("exact-multiple");

  /* Not a multiple: the LAST chunk is short by construction, the one case a
     "short read means failure" rule must not reject. */
  reset(256);
  r = psram_stream_buf(&file_handle, PS_BASE, 40, buf, 16, test_pump);
  check_eq("short-last-chunk", "return", r, 1);
  check_eq("short-last-chunk", "pump calls", pumps, 3);
  check_log("short-last-chunk", "R16 W16 P R16 W16 P R8 W8 P");
  check_psram("short-last-chunk", 40);
  pass("short-last-chunk");

  /* A NULL pump is the normal case outside the menu-SFX paths. */
  reset(256);
  r = psram_stream_buf(&file_handle, PS_BASE, 32, buf, 16, NULL);
  check_eq("no-pump", "return", r, 1);
  check_log("no-pump", "R16 W16 R16 W16");
  check_psram("no-pump", 32);
  pass("no-pump");

  /* The file ends early: f_read returns FR_OK with a short count, so got !=
     want is the only signal, and the region must be left partially written and
     never claimed complete.  file_res stays FR_OK -- the read did not fail. */
  reset(20);
  r = psram_stream_buf(&file_handle, PS_BASE, 40, buf, 16, test_pump);
  check_eq("file-too-short", "return", r, 0);
  check_eq("file-too-short", "file_res", file_res, FR_OK);
  check_eq("file-too-short", "pump calls", pumps, 1);
  check_log("file-too-short", "R16 W16 P R16");
  check_psram("file-too-short", 16);
  pass("file-too-short");

  /* A real read error on the second chunk: stop there, publish the FRESULT. */
  reset(256);
  sd_fail_call = 2;
  sd_fail_res = FR_DISK_ERR;
  r = psram_stream_buf(&file_handle, PS_BASE, 40, buf, 16, test_pump);
  check_eq("read-error", "return", r, 0);
  check_eq("read-error", "file_res", file_res, FR_DISK_ERR);
  check_eq("read-error", "pump calls", pumps, 1);
  check_log("read-error", "R16 W16 P R16");
  check_psram("read-error", 16);
  pass("read-error");

  /* Failure on the very first chunk writes nothing at all. */
  reset(256);
  sd_fail_call = 1;
  sd_fail_res = FR_INT_ERR;
  r = psram_stream_buf(&file_handle, PS_BASE, 40, buf, 16, test_pump);
  check_eq("read-error-first", "return", r, 0);
  check_eq("read-error-first", "file_res", file_res, FR_INT_ERR);
  check_log("read-error-first", "R16");
  check_psram("read-error-first", 0);
  pass("read-error-first");

  /* A SUCCESSFUL stream does not touch file_res: a caller reading it after a
     stream that worked gets whatever the previous operation left, not FR_OK. */
  reset(256);
  r = psram_stream_buf(&file_handle, PS_BASE, 32, buf, 16, NULL);
  check_eq("success-leaves-file_res", "return", r, 1);
  check_eq("success-leaves-file_res", "file_res", file_res, FR_INVALID_PARAMETER);
  pass("success-leaves-file_res");

  /* psram_stream() is the same over the shared file_buf: the chunk sizes ARE
     the assertion that it passes sizeof(file_buf). */
  reset(2048);
  r = psram_stream(&file_handle, PS_BASE, 1000, test_pump);
  check_eq("stream-file_buf", "return", r, 1);
  check_eq("stream-file_buf", "pump calls", pumps, 2);
  check_log("stream-file_buf", "R512 W512 P R488 W488 P");
  check_psram("stream-file_buf", 1000);
  check("stream-file_buf", sizeof file_buf == 512, "file_buf is no longer 512 bytes");
  pass("stream-file_buf");

  if (fails) {
    printf("== summary: %d failure(s) ==\n", fails);
    return 1;
  }
  printf("== summary: all psram_io cases pass ==\n");
  return 0;
}
