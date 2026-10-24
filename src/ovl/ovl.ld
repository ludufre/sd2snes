/* Card overlay: linked at the firmware's ptrcache, against the firmware's symbols
   (-Wl,--just-symbols), see ovl.h */
SECTIONS
{
  .text :
  {
    __ovl_start = .;
    KEEP(*(.ovlhdr))
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4);
    __ovl_end = .;
  }
  /* transient AHB scratch of the overlaid files: right after the image, inside the block */
  .ahbram (NOLOAD) : { *(.ahbram .ahbram.*) . = ALIGN(4); __ovl_scratch_end = .; }
  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
  .data : { *(.data .data.*) }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
ASSERT(SIZEOF(.data) == 0, "overlay: initialised writable data not allowed")
ASSERT(SIZEOF(.bss) <= __ovl_bss_max, "overlay: .bss outgrew its slice of ovl_bss")
