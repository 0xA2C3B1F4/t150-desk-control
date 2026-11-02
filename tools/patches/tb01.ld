ENTRY(tb01_route)
/* Same unsigned quotient/remainder helper used by native RTC scheduling. */
__aeabi_uidiv = 0x080028f5;
SECTIONS {
  .hook_sound 0x08003408 : { *(.hook_sound) }
  .hook_sleep 0x0800865e : { *(.hook_sleep) }
  .hook_init  0x08009402 : { *(.hook_init) }
  .hook_route 0x08009434 : { *(.hook_route) }
  .hook_tick  0x08009452 : { *(.hook_tick) }
  .candidate 0x0800a010 : { *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.ARM.exidx*) *(.ARM.extab*) }
}
ASSERT(SIZEOF(.candidate)<=0x7f0,"TB01 exceeds existing 32 KiB install payload")
ASSERT(SIZEOF(.hook_sound)==8,"native sound hook size")
ASSERT(SIZEOF(.hook_init)==4 && SIZEOF(.hook_route)==4 && SIZEOF(.hook_tick)==4 && SIZEOF(.hook_sleep)==4,"hook size")
ASSERT(SIZEOF(.data)==0 && SIZEOF(.bss)==0,"unexpected RAM data")
