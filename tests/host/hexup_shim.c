/* f_printf (src/ff.c) renders with printf.c's vsnprintf_hexup; suites that link the real
   FatFs but not printf.c (run_fileops.sh) get it from the host libc, whose %X is upper case
   as well.  run_fprintf.sh tests the real pair. */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
int vsnprintf_hexup(char *s, size_t n, const char *f, va_list ap) { return vsnprintf(s, n, f, ap); }
