/* sd2snes - small string/path helpers shared across the firmware */

#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stddef.h>

/* Bounded copy that ALWAYS terminates: at most dstsz-1 bytes of src plus the NUL.
 * dstsz == 0 touches nothing. Returns strlen(src) (the BSD contract), so `>= dstsz`
 * means it truncated. src MUST be NUL-terminated: unlike strncpy it is scanned to
 * its end even when dst fills up first.
 *
 * NOT a drop-in for every strncpy: strncpy pads the rest of dst with NULs, this does
 * not. Where the WHOLE buffer travels somewhere -- a struct blitted to the shared
 * BSRAM, a block pushed over USB, a buffer compared with memcmp -- that padding is
 * load-bearing; keep strncpy, or zero the buffer first. */
size_t strlcpy_nul(char *dst, const char *src, size_t dstsz);

/* Last path component: everything after the final '/', or the whole string when there
 * is none.  Callers that want the SLASH itself (to cut a directory prefix, to compare
 * parents) keep their own strrchr -- this returns the leaf only. */
const char *path_leaf(const char *path);

#endif
