/* The System Information screen's loop.  Resident (the block refresh, sysinfo.c, is card
   overlay code): the loop serves USB, and a USB boot runs load_rom, which may load a
   session module over the overlay -- the overlay code must not be on the stack then. */
#include "config.h"
#include "snes.h"
#include "timer.h"
#include "usbinterface.h"
#include "msu1.h"
#include "sysinfo.h"

void sysinfo_loop(void) {
  int v2, sd_measured = 0;
  echo_mcu_cmd();
  /* sysinfo measures raw SD access times, which races a playing effect's
     sd_offload streaming (the DAC then loops stale buffer content - a stuck
     tone for the whole measurement). Cut any effect cleanly on entry; the
     screen has no sounds of its own. */
  menu_sfx_stop();
  v2 = sysinfo_open();
  while(snes_get_mcu_cmd() == SNES_CMD_SYSINFO) {
    if(v2) {
      sd_measured = sysinfo_pass(sd_measured);
    }
    delay_ms(100);
    usbint_handler();
  }
  echo_mcu_cmd();
}
