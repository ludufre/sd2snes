/* Host shim for src/ovl.h: sort.c forgets the card overlay before it reuses ptrcache; the
   host has no overlay, so there is nothing to forget */
#ifndef HOST_OVL_H
#define HOST_OVL_H
static inline void ovl_forget(void) { }
#endif
