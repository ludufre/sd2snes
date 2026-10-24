/**
 * \file crc32.c
 * Functions and types for CRC checks.
 *
 * Generated on Sat Sep 25 18:06:34 2010,
 * by pycrc v0.7.1, http://www.tty1.net/pycrc/
 * using the configuration:
 *    Width        = 32
 *    Poly         = 0x04c11db7
 *    XorIn        = 0xffffffff
 *    ReflectIn    = True
 *    XorOut       = 0xffffffff
 *    ReflectOut   = True
 *    Algorithm    = table-driven
 *    Direct       = True
 *****************************************************************************/
#include "crc32.h"
#include "config.h"
#include <stdint.h>

/**
 * Static table used for the table_driven implementation.
 *****************************************************************************/
/* 16-entry (nibble) table: 64 B instead of 1 KB, two lookups per byte. */
static const uint32_t crc32_table[16] = {
  0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
  0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
};

/**
 * Update the crc value with new data.
 *
 * \param crc      The current crc value.
 * \param data     Pointer to a buffer of \a data_len bytes.
 * \param data_len Number of bytes in the \a data buffer.
 * \return         The updated crc value.
 *****************************************************************************/
uint32_t crc32_update(uint32_t crc, const unsigned char data)
{
  unsigned int tbl_idx;

  tbl_idx = (crc ^ data) & 0x0f;
  crc = crc32_table[tbl_idx] ^ (crc >> 4);
  tbl_idx = (crc ^ (data >> 4)) & 0x0f;
  crc = crc32_table[tbl_idx] ^ (crc >> 4);

  return crc & 0xffffffff;
}


