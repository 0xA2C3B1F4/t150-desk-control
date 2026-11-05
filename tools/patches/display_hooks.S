.syntax unified
.cpu cortex-m0
.thumb
.section .hook_display_render,"ax",%progbits
  bl display_controller_render
.section .hook_display_stop,"ax",%progbits
  bl display_native_stop
.section .text.display_native_stop,"ax",%progbits
.global display_native_stop
.thumb_func
display_native_stop:
  push {r0-r4,lr}
  mov r4, r12
#ifdef DESK_LOCAL_PANEL_LOCK
  ldrb r0, [r2,#3]
  cmp r0, #0
  bne 1f
  bl local_lock_native_stop
  mov r12, r4
  /* Both Stop producers use one semantic routine. Continue at native ACK,
   * past the old inline brake so it cannot run for a second time. */
  ldr r0, =0x08007e3f
  str r0, [sp,#20]
  pop {r0-r4,pc}
1:
#endif
  bl display_controller_native_stop
  mov r12, r4
  /* Replay LDRB/CMP with the original request pointer and preserve its flags. */
  ldr r0, [sp,#8]
  ldrb r0, [r0,#3]
  str r0, [sp,#0]
  cmp r0, #1
  pop {r0-r4,pc}
#ifdef DESK_LOCAL_PANEL_LOCK
.balign 4
.ltorg
#endif
