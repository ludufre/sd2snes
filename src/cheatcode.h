/* sd2snes -- cheat code text <-> binary conversion.

   Pure functions (no firmware includes) so the host tests can link the REAL
   Game Genie decoder next to the code under test.  Two text forms are accepted
   everywhere a code is typed or read from a .yml:

     PAR / raw   "AAAAAADD"   8 hex digits: 24-bit bus address, 8-bit value
     Game Genie  "XXXX-YYYY"  8 hex digits with a '-' after the 4th

   The SNES Game Genie alphabet (DF4709156BC8A23E) is a permutation of the 16
   hex digits, so a GG code is read as hex and each nibble is remapped
   (cheat_gg2raw).  cheat_str2bin keeps the historical lenient parse; the
   editor path validates with cheat_code_valid FIRST, because strtoul turns
   garbage into a silent zero code. */

#ifndef CHEATCODE_H
#define CHEATCODE_H

#include <stdint.h>

/* lenient: GG when strlen >= 9 and s[4] == '-', else hex via strtoul */
uint32_t cheat_str2bin(char *string);

/* Game Genie <-> raw (value | addr<<8 | bank<<24) */
uint32_t cheat_gg2raw(uint32_t code);
uint32_t cheat_raw2gg(uint32_t code);

/* strict: 1 when s is exactly 8 hex digits, or 9 chars with '-' at [4] and
   8 hex digits around it (either case); 0 otherwise */
int cheat_code_valid(const char *s);

#endif
