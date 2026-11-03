.syntax unified
.cpu cortex-m0
.thumb
.section .hook_boot,"ax",%progbits
  bl platform_startup
.section .text.platform_startup,"ax",%progbits
.global platform_startup
.thumb_func
platform_startup:
  /* Keep the original main BL ABI. The extra frame is gone before the
   * native vector routine, and PRIMASK/APSR are restored exactly. */
  push {r0-r7,lr}
  sub sp, #12
  mrs r0, APSR
  str r0, [sp, #0]
  mrs r0, PRIMASK
  str r0, [sp, #4]
  mov r0, r12
  str r0, [sp, #8]
  cpsid i
  /* Invalidate the owned RS01 magic before publishing its SysTick handler.
   * rs_boot initializes the complete RS01 state later in the native order. */
  movs r0, #0
  ldr r1, =0x20000fe4
  str r0, [r1]
  bl platform_bootstrap
  ldr r0, [sp, #4]
  msr PRIMASK, r0
  ldr r0, [sp, #8]
  mov r12, r0
  ldr r0, [sp, #44]
  mov lr, r0
  ldr r0, =0x08004a99
  str r0, [sp, #44]
  ldr r0, [sp, #0]
  msr APSR_nzcvq, r0
  add sp, #12
  pop {r0-r7,pc}
.ltorg
