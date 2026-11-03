INCLUDE platform_layout.ld
ENTRY(platform_startup)
__aeabi_uidiv = 0x080028f5;
SECTIONS {
  .hook_sound 0x08003408 : { *(.hook_sound) }
  .hook_boot 0x080085f4 : { *(.hook_boot) }
  .hook_sleep 0x0800865e : { *(.hook_sleep) }
  .hook_init 0x08009402 : { *(.hook_init) }
  .hook_route 0x08009434 : { *(.hook_route) }
  .hook_tick 0x08009452 : { *(.hook_tick) }
  .candidate PLATFORM_CODE_START : { *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.ARM.exidx*) *(.ARM.extab*) }
}
ASSERT(ADDR(.candidate) + SIZEOF(.candidate) <= PLATFORM_CODE_END, "platform flash overflow")
ASSERT(SIZEOF(.hook_sound) == 8, "sound hook size")
ASSERT(SIZEOF(.hook_boot) == 4 && SIZEOF(.hook_sleep) == 4 && SIZEOF(.hook_init) == 4 && SIZEOF(.hook_route) == 4 && SIZEOF(.hook_tick) == 4, "hook size")
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "unexpected RAM data")
