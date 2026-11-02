.syntax unified
.cpu cortex-m0
.thumb
.section .hook_init,"ax",%progbits
  bl tb01_init
.section .hook_route,"ax",%progbits
  bl tb01_route
.section .hook_tick,"ax",%progbits
  bl tb01_watchdog
.section .hook_sleep,"ax",%progbits
  bl tb01_sleep
.section .hook_sound,"ax",%progbits
  ldr r3, 1f
  bx r3
1: .word tb01_native_sound
.section .text.tb01_native_sound,"ax",%progbits
.global tb01_native_sound
.thumb_func
tb01_native_sound:
  /* Preempt our sequence when native sound actually claims the output.
   * Preserve arguments r0/r1, callee-saved registers and APSR. The absolute
   * entry jump already used caller-saved r3; the native body has no r3 input. */
  push {r0,r1,r2,r3,r4,lr}
  mrs r4, APSR
  bl tb01_native_priority
  msr APSR_nzcvq, r4
  pop {r0,r1,r2,r3,r4}
  pop {r3}
  mov lr, r3
  /* Entry's original r3 is caller-saved and the stock prologue saves it only
   * as an alignment slot. It is overwritten before use by the stock body. */
  push {r3,r4,r5,r6,r7,lr}
  ldr r5, =0x200008a8
  mov r4, r0
  mov r7, r1
  ldr r3, =0x08003411
  bx r3
.ltorg
