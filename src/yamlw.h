/* sd2snes - YAML sidecar writer shared by cheats and patch metadata */

#ifndef YAMLW_H
#define YAMLW_H

#include <stdint.h>

#include "ff.h"

/* Create (or re-create) a per-ROM YAML sidecar at `path` and write its document
   header.  Releases the FPGA chip select first -- a prior SPI transaction that left
   it asserted corrupts the card's traffic -- creates the bucket directory only now
   that the name is built, clears any read-only/hidden/system attribute, unlinks and
   opens truncating, with an open-always + f_truncate fallback.

   Returns file_res: non-zero means nothing is open and the caller must not write.
   file_handle is the open file on success. */
FRESULT yaml_open_write(char *path);

/* Write `prefix` (which ends in the opening double quote of a YAML scalar),
   then s with the entity escaping yaml_decode_entities undoes, then the closing
   quote and a newline. */
void yaml_put_quoted(FIL *fp, const char *prefix, const char *s);

#endif
