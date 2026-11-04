SECTIONS {
  .hook_display_stop 0x08007d76 : { *(.hook_display_stop) }
  .hook_display_render 0x08008678 : { *(.hook_display_render) }
}
INSERT BEFORE .hook_trim_uart;
INCLUDE buzzer_patch.ld
ASSERT(SIZEOF(.hook_display_stop) == 4, "display stop hook size")
ASSERT(SIZEOF(.hook_display_render) == 4, "display render hook size")
