/* sd2snes - small string/path helpers shared across the firmware. See util.h.
 *
 * Deliberately free of firmware dependencies (no config.h, no ff.h) so
 * tests/host/run_strutil.sh can compile it straight from src/. Keep it that way. */

#include <string.h>
#include "util.h"

size_t strlcpy_nul(char *dst, const char *src, size_t dstsz) {
  size_t n = 0;
  if(dstsz) {
    /* copy the head that fits, then terminate -- no tail padding (see util.h) */
    while(n < dstsz - 1 && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = 0;
  }
  while(src[n]) n++;                     /* finish measuring src for the return value */
  return n;
}

const char *path_leaf(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}
