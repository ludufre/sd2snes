/* Host shim for src/fpga_spi.h.
 *
 * patch.c's psram_* helpers drive the FPGA<->SDRAM window with a fixed
 * protocol: SELECT; TX(SETADDR|TGT_MEM); TX(addr23..16); TX(addr15..8);
 * TX(addr7..0); DESELECT -- then SELECT; TX(0x98 write / 0x88 read);
 * per-byte TX/RX with auto-increment. The host fakes exactly that protocol
 * with a tiny state machine over a 16 MB array (shim.c), so patch.c compiles
 * and runs UNMODIFIED. Addresses wrap at 24 bits like the real window. */
#ifndef HOST_FPGA_SPI_H
#define HOST_FPGA_SPI_H

#include <stdint.h>

#define FPGA_CMD_SETADDR 0x10
#define FPGA_TGT_MEM     0x00

void    host_fpga_select(void);
void    host_fpga_deselect(void);
void    host_fpga_tx(uint8_t b);
uint8_t host_fpga_rx(void);

#define FPGA_SELECT()    host_fpga_select()
#define FPGA_DESELECT()  host_fpga_deselect()
#define FPGA_TX_BYTE(b)  host_fpga_tx((uint8_t)(b))
#define FPGA_RX_BYTE()   host_fpga_rx()
#define FPGA_WAIT_RDY()         do { } while (0)
#define FPGA_WAIT_RDY_TO(err)   do { } while (0)

#endif
