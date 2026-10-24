/* strtok() for newlib-nano.  The nano strtok keeps its continuation pointer in a block it
   allocates on first use (_REENT_CHECK_MISC), and that drags malloc, assert and stdio --
   about 4 KB and the syscalls the firmware does not have -- into the image.  The same
   function over strtok_r with one hidden position, exactly the contract of the C library's
   (yaml.c and savestate.c rely on that single shared position). */
#include <string.h>

char *strtok(char *s, const char *delim) {
  static char *save;
  return strtok_r(s, delim, &save);
}
