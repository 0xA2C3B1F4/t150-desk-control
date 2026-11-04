INCLUDE platform_layout.ld
ENTRY(platform_startup)
__aeabi_uidiv = 0x080028f5;
__aeabi_uidivmod = __aeabi_uidiv;
trim_original_dispatch = 0x0800942d;
trim_original_reset_scanners = 0x08008cd9;
SECTIONS {
  .hook_sound 0x08003408 : { *(.hook_sound) }
  .hook_trim_tx 0x08003610 : { *(.hook_trim_tx) }
  .hook_trim_gpio 0x0800438c : { *(.hook_trim_gpio) }
  .hook_trim_preset_1 0x08004e82 : { *(.hook_trim_preset_1) }
  .hook_trim_preset_2 0x08004f9a : { *(.hook_trim_preset_2) }
  .hook_trim_preset_3 0x080050b2 : { *(.hook_trim_preset_3) }
  .hook_trim_native 0x080058d0 : { *(.hook_trim_native) }
  .hook_trim_uart 0x08007bae : { *(.hook_trim_uart) }
  .hook_boot 0x080085f4 : { *(.hook_boot) }
  .hook_trim_startup 0x08008636 : { *(.hook_trim_startup) }
  .hook_trim_selector 0x0800863e : { *(.hook_trim_selector) }
  .hook_sleep 0x0800865e : { *(.hook_sleep) }
  .hook_trim_foreground 0x08008684 : { *(.hook_trim_foreground) }
  .hook_trim_direct 0x0800899e : { *(.hook_trim_direct) }
  .hook_init 0x08009402 : { *(.hook_init) }
  .hook_route 0x08009434 : { *(.hook_route) }
  .hook_tick 0x08009452 : { *(.hook_tick) }
  .candidate PLATFORM_CODE_START : { *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.ARM.exidx*) *(.ARM.extab*) }
}
ASSERT(ADDR(.candidate) + SIZEOF(.candidate) <= PLATFORM_CODE_END, "platform flash overflow")
ASSERT(SIZEOF(.hook_sound) == 8, "sound hook size")
ASSERT(SIZEOF(.hook_boot) == 4 && SIZEOF(.hook_sleep) == 4 && SIZEOF(.hook_init) == 4 && SIZEOF(.hook_route) == 4 && SIZEOF(.hook_tick) == 4, "platform hook size")
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "unexpected RAM data")
ASSERT(SIZEOF(.hook_trim_preset_1) == 22 && SIZEOF(.hook_trim_preset_2) == 22 && SIZEOF(.hook_trim_preset_3) == 22 && SIZEOF(.hook_trim_direct) == 22, "trim target hook size")
ASSERT(ADDR(.hook_trim_preset_1) % 4 == 2 && ADDR(.hook_trim_preset_2) % 4 == 2 && ADDR(.hook_trim_preset_3) % 4 == 2 && ADDR(.hook_trim_direct) % 4 == 2, "trim target literal alignment")
ASSERT(SIZEOF(.hook_trim_tx) == 8 && SIZEOF(.hook_trim_uart) == 4 && SIZEOF(.hook_trim_native) == 12, "trim storage hook size")
ASSERT(SIZEOF(.hook_trim_gpio) == 4 && SIZEOF(.hook_trim_selector) == 4 && SIZEOF(.hook_trim_foreground) == 4, "trim foreground hook size")
ASSERT(SIZEOF(.hook_trim_startup) == 4, "trim startup hook size")
