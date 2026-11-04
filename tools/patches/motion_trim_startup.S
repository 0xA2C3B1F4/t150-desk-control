.syntax unified
.cpu cortex-m0
.thumb

.section .hook_trim_startup,"ax",%progbits
  bl trim_startup

.section .text.trim_startup,"ax",%progbits
.global trim_startup
.thumb_func
trim_startup:
  /* Native initialization and CL01 loading have finished. Preserve the GPIO
   * result in r0 for the native store at863a and replay both displaced loads. */
  push {r0-r3,r6,r7,lr}
  sub sp, #12
  mrs r0, APSR
  str r0, [sp, #0]
  mov r0, r12
  str r0, [sp, #4]
  bl trim_controller_boot
  ldr r4, =0x20000754
  ldr r5, =0x48001400
  ldr r0, [sp, #4]
  mov r12, r0
  ldr r0, [sp, #0]
  msr APSR_nzcvq, r0
  add sp, #12
  pop {r0-r3,r6,r7,pc}
.balign 4
.ltorg
