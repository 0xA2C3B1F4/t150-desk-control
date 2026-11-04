.syntax unified
.cpu cortex-m0
.thumb

.section .hook_trim_uart,"ax",%progbits
  bl trim_dispatch

.section .hook_trim_tx,"ax",%progbits
  ldr r3, [pc,#0]
  bx r3
  .word trim_tx

.section .text.trim_dispatch,"ax",%progbits
.global trim_dispatch
.thumb_func
trim_dispatch:
  /* The native dispatcher has already saved r3-r7/LR. A recognized request
   * uses its handled return. Pass-through restores the original entry state. */
  push {r0-r4,lr}
  mrs r0, APSR
  mov r1, r12
  push {r0,r1}
  bl trim_controller_route
  cmp r0, #0
  bne 1f
  pop {r0,r1}
  msr APSR_nzcvq, r0
  mov r12, r1
  ldr r0, [sp,#20]
  mov lr, r0
  /* R7's complete dispatcher prefix remains at its bound entry. */
  ldr r0, =trim_original_dispatch
  str r0, [sp,#20]
  pop {r0-r4,pc}
1:
  add sp, #32
  pop {r3-r7,pc}
  .balign 4
  .ltorg

.section .text.trim_tx,"ax",%progbits
.global trim_tx
.thumb_func
trim_tx:
  push {r0-r2,r4-r7,lr}
  mrs r4, APSR
  push {r4,r5}
  bl trim_controller_tx_reserved
  cmp r0, #0
  beq 2f
  ldr r0, [sp,#8]
  ldr r1, [sp,#12]
  bl trim_controller_send_native
  str r0, [sp,#8]
  pop {r4,r5}
  pop {r0-r2,r4-r7,pc}
2:
  pop {r4,r5}
  msr APSR_nzcvq, r4
  pop {r0-r2,r4-r7}
  pop {r3}
  mov lr, r3
  /* Reproduce the displaced native prologue. R4 is dead until its next LDR
   * at 361A and its caller value is already in the native five-word frame. */
  push {r4-r7,lr}
  mov r12, r1
  ldr r1, [r1]
  ldrb r3, [r1]
  ldr r4, =0x08003619
  bx r4
  .balign 4
  .ltorg
