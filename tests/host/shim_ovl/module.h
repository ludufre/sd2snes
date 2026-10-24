/* Host shim for src/module.h: the two calls the overlay loader makes (ovl_cli.c) */
#ifndef HOST_OVL_MODULE_H
#define HOST_OVL_MODULE_H
#include <stdint.h>
int  module_live(void);
void module_gate(uint8_t cmd);
#endif
