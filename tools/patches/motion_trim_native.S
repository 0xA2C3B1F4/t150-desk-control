.syntax unified
.cpu cortex-m0
.thumb

.section .hook_trim_native,"ax",%progbits
  /* Preserve the exact native frame before borrowing r3 for the entry jump. */
  push {r3,lr}
  ldr r3, 1f
  bx r3
  nop
1: .word trim_native_entry

.section .text.trim_native_entry,"ax",%progbits
.global trim_native_entry
.thumb_func
trim_native_entry:
  movs r0, #1
  msr PRIMASK, r0
  /* Keep the call aligned and retain every volatile input used by the native
   * continuation. Its original r3 and return address remain in the hook frame. */
  push {r1,r2,r4,r5,r6,lr}
  mov r4, r12
  mrs r5, APSR
  ldr r6, =0x2000088d
  ldrb r6, [r6]
  cmp r6, #2
  bne 3f
  movs r6, #10
3:
  bl trim_controller_capture_native
  cmp r0, #1                 /* TRIM_CAPTURE_PASSTHROUGH */
  bne 2f

  mov r12, r4
  msr APSR_nzcvq, r5
  pop {r1,r2,r4,r5,r6}
  pop {r3}
  mov lr, r3
  ldr r0, =0x200007d4
  ldr r0, [r0]
  ldr r3, =0x080058dd
  push {r3}
  ldr r3, [sp,#4]
  pop {pc}

2:
  /* Native STRH scratch stores replace the saved r3 low halfword with mode. */
  mov r1, sp
  strh r6, [r1,#24]
  mov r12, r4
  pop {r1,r2,r4,r5,r6}
  add sp, #4
  /* Both queued and retained captures preserve 58D0's unconditional unmask.
   * Callers still execute their existing mode/rearm stores after this return. */
  movs r0, #0
  msr PRIMASK, r0
  pop {r3,pc}
.balign 4
.ltorg
