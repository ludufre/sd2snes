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
#define FPGA_WAIT_RDY_TO(err)          do { if (host_fpga_wait_to()) (err) = 1; } while (0)
/* The _INLINE spellings are the same waits kept as macros inside per-byte loops. */
#define FPGA_WAIT_RDY_INLINE()         do { } while (0)
#define FPGA_WAIT_RDY_TO_INLINE(err)   FPGA_WAIT_RDY_TO(err)

/* ---- MCU_RDY stall injection -------------------------------------------
 * The bounded waits are the only thing that can raise patch_io_err, so without
 * injection every "if (patch_io_err) ..." path in patch.c is unreachable here.
 * host_fpga_fault_after counts bounded waits down: 0 disables it, N makes the
 * Nth wait time out.  The stall then LATCHES -- a deasserted MCU_RDY does not
 * come back -- so code that clears patch_io_err mid-apply cannot "recover".
 * The writes/reads-after-fault counters are the bytes moved through the window
 * after the latch: the patcher must abort rather than keep driving an FPGA
 * that is not acknowledging.  Reads count too, since a read loop that ignores
 * the latch corrupts nothing and would leave no other trace.
 * host_fpga_wait_count() is the total, so a caller can aim the fault at a wait
 * that exists. */
extern unsigned host_fpga_fault_after;
int      host_fpga_wait_to(void);            /* 1 = this wait timed out */
unsigned host_fpga_wait_count(void);
unsigned host_fpga_writes_after_fault(void);
unsigned host_fpga_reads_after_fault(void);

#endif
