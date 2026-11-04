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
  bl display_controller_native_stop
  mov r12, r4
  /* Replay LDRB/CMP with the original request pointer and preserve its flags. */
  ldr r0, [sp,#8]
  ldrb r0, [r0,#3]
  str r0, [sp,#0]
  cmp r0, #1
  pop {r0-r4,pc}
