/* sd2snes -- cheat code text <-> binary conversion. See cheatcode.h. */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "cheatcode.h"

static int cc_ishex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int cheat_code_valid(const char *s) {
  int len = 0;
  if(!s) return 0;
  while(s[len]) len++;
  if(len == 8) {
    for(int i = 0; i < 8; i++) if(!cc_ishex(s[i])) return 0;
    return 1;
  }
  if(len == 9 && s[4] == '-') {
    for(int i = 0; i < 9; i++) if(i != 4 && !cc_ishex(s[i])) return 0;
    return 1;
  }
  return 0;
}

uint32_t cheat_str2bin(char *string) {
  char code[9];
  uint32_t patch;
  if(strlen(string) >= 9 && string[4] == '-') {
    /* GG code */
    memcpy(code, string, 4);
    strncpy(code+4, string+5, 4);
    code[8] = 0;
    patch = (uint32_t)strtoul(code, NULL, 16);
    patch = cheat_gg2raw(patch);
  } else {
    /* PAR/RAW code */
    patch = (uint32_t)strtoul(string, NULL, 16);
  }
  return patch;
}

uint32_t cheat_gg2raw(uint32_t patch) {
  uint8_t gg2raw_tab[16] = {
    0x4, 0x6, 0xd, 0xe,
    0x2, 0x7, 0x8, 0x3,
    0xb, 0x5, 0xc, 0x9,
    0xa, 0x0, 0xf, 0x1
  };
  uint32_t decrypt = 0;
  /* translate nibbles */
  for(int i=0; i<8; i++) {
    decrypt = ((decrypt >> 4) & 0x0fffffff)
            | ((uint32_t)(gg2raw_tab[patch & 0xf]) << 28);
    patch >>= 4;
  }
  /* remap bits: VVVVVVVVAAAABBBBCCDDDDEEEEFFFFGG
              => DDDDFFFFAAAAGGCCBBBBEEEEVVVVVVVV */
  decrypt = ((decrypt & 0xff000000) >> 24)
          |  (decrypt & 0x00f00000)
          | ((decrypt & 0x000f0000) >> 4)
          | ((decrypt & 0x0000c000) << 2)
          | ((decrypt & 0x00003c00) << 18)
          | ((decrypt & 0x000003c0) << 2)
          | ((decrypt & 0x0000003c) << 22)
          | ((decrypt & 0x00000003) << 18);
  return decrypt;
}

uint32_t cheat_raw2gg(uint32_t patch) {
  uint8_t raw2gg_tab[16] = {
    0xd, 0xf, 0x4, 0x7,
    0x0, 0x9, 0x1, 0x5,
    0x6, 0xb, 0xc, 0x8,
    0xa, 0x2, 0x3, 0xe
  };
  uint32_t encrypt = 0;
  /* remap bits: AAAABBBBCCCCDDEEFFFFGGGGVVVVVVVV
              => VVVVVVVVCCCCFFFFEEAAAAGGGGBBBBDD */
  patch = ((patch & 0xf0000000) >> 18)
        | ((patch & 0x0f000000) >> 22)
        |  (patch & 0x00f00000)
        | ((patch & 0x000c0000) >> 18)
        | ((patch & 0x00030000) >> 2)
        | ((patch & 0x0000f000) << 4)
        | ((patch & 0x00000f00) >> 2)
        | ((patch & 0x000000ff) << 24);
  /* translate nibbles */
  for(int i=0; i<8; i++) {
    encrypt = ((encrypt >> 4) & 0x0fffffff)
            | ((uint32_t)(raw2gg_tab[patch & 0xf]) << 28);
    patch >>= 4;
  }
  return encrypt;
}
