/**
 * \file stdout
 * Functions and types for CRC checks.
 *
 * Generated on Tue Sep 15 09:32:35 2009,
 * by pycrc v0.7.1, http://www.tty1.net/pycrc/
 * using the configuration:
 *    Width        = 16
 *    Poly         = 0x8005
 *    XorIn        = 0xffff
 *    ReflectIn    = True
 *    XorOut       = 0xffff
 *    ReflectOut   = True
 *    Algorithm    = bit-by-bit-fast
 *    Direct       = True
 *****************************************************************************/
#include "crc16.h"
#include <stdint.h>


/**
 * Update the crc value with new data.
 *
 * \param crc      The current crc value.
 * \param data     Pointer to a buffer of \a data_len bytes.
 * \param data_len Number of bytes in the \a data buffer.
 * \return         The updated crc value.
 *****************************************************************************/
uint16_t crc16_update(uint16_t crc, const unsigned char data)
{
  /* Bit-by-bit form of the reflected CRC-16 step, identical to the 256-entry table
     and much cheaper in flash.  The only caller runs it once per game load. */
  int i;
  crc ^= data;
  for(i = 0; i < 8; i++) {
    crc = (crc & 1) ? ((crc >> 1) ^ 0xa001) : (crc >> 1);
  }
  return crc;
}


