/* sd2snes - bounded SD file -> PSRAM streaming. See psram_io.h. */

#include "config.h"
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "psram_io.h"

int psram_stream_buf(FIL *fp, uint32_t addr, uint32_t size, void *buf,
                     uint16_t bufsz, void (*pump)(void)) {
  UINT got;
  if(!bufsz) return 0;   /* a zero-sized buffer would never advance: refuse, never spin */
  while(size) {
    UINT want = (size > bufsz) ? (UINT)bufsz : (UINT)size;
    FRESULT res = f_read(fp, buf, want, &got);
    if(res || got != want) { file_res = res; return 0; }
    sram_writeblock(buf, addr, (uint16_t)got);
    addr += got;
    size -= got;
    if(pump) pump();
  }
  return 1;
}

int psram_stream(FIL *fp, uint32_t addr, uint32_t size, void (*pump)(void)) {
  return psram_stream_buf(fp, addr, size, file_buf, sizeof(file_buf), pump);
}
