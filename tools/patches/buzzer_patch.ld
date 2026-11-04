SECTIONS {
  .hook_buzzer_timer 0x080067ec : { *(.hook_buzzer_timer) }
}
INSERT BEFORE .hook_trim_uart;
INCLUDE motion_trim_patch.ld
ASSERT(SIZEOF(.hook_buzzer_timer) == 8, "timer profile hook size")
