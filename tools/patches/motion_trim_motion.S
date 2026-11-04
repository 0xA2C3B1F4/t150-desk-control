.syntax unified
.cpu cortex-m0
.thumb

/* All four exact-image spans start at address 2 modulo 4. The literal is
 * therefore aligned at offset 6. Use the explicit PC offset rather than an
 * assembler alignment directive that assumes a word-aligned section base. */
.macro target_entry name
.section .hook_trim_\name,"ax",%progbits
  push {r3,lr}
  ldr r3, [pc,#0]
  bx r3
  .word trim_\name
  .rept 6
    nop
  .endr
.endm

target_entry preset_1
target_entry preset_2
target_entry preset_3
target_entry direct

.macro target_body name, continuation, preset
.section .text.trim_\name,"ax",%progbits
.global trim_\name
.thumb_func
trim_\name:
  /* Entry saved original r3/LR. This makes a 48-byte aligned frame, preserves
   * r12 across C, and returns with the original LR instead of the hook address. */
  push {r0-r2,r4-r7,lr}
  mrs r4, APSR
  mov r5, r12
  push {r4,r5}
.if \preset
  ldrh r1, [r1,#10]
.else
  ldr r0, [sp,#48]
  ldr r1, [sp,#64]
.endif
  bl trim_motion_target
  mov r6, r0

  /* Reproduce the original scratch registers and condition flags. The preset
   * dead zone skips the second target store exactly as the native code does. */
.if \preset
  ldr r0, [sp,#8]
  ldr r1, [sp,#12]
  ldrh r1, [r1,#10]
.else
  ldr r0, [sp,#48]
  ldr r1, [sp,#64]
.endif
  adds r2, r1, #4
  cmp r0, r2
  ble 1f
  adds r0, #7
  b 2f
1:
  subs r1, #4
  cmp r0, r1
  bge 3f
  subs r0, #6
2:
  mov r0, r6
.if \preset
  /* r4 still belongs to the caller: the C ABI preserves it, but our prologue
   * used it for APSR. Reload its saved value before the original store. */
  ldr r3, [sp,#20]
  str r0, [r3,#12]
.endif
3:
  mrs r3, APSR
  str r3, [sp,#0]
  str r0, [sp,#8]
  str r1, [sp,#12]
  str r2, [sp,#16]
  ldr r3, =\continuation
  str r3, [sp,#44]
  pop {r4,r5}
  msr APSR_nzcvq, r4
  mov r12, r5
  pop {r0-r2,r4-r7}
  pop {r3}
  mov lr, r3
  pop {r3,pc}
  .balign 4
  .ltorg
.endm

target_body preset_1, 0x08004e99, 1
target_body preset_2, 0x08004fb1, 1
target_body preset_3, 0x080050c9, 1
target_body direct, 0x080089b5, 0
