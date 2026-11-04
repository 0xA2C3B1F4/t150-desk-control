.syntax unified
.cpu cortex-m0
.thumb

.section .hook_trim_selector,"ax",%progbits
  bl trim_selector
.section .hook_trim_foreground,"ax",%progbits
  bl trim_foreground
.section .hook_trim_gpio,"a",%progbits
  .word trim_gpio

.section .text.trim_selector,"ax",%progbits
.global trim_selector
.thumb_func
trim_selector:
  push {r0,r2,r3,r4,r5,lr}
  mov r5, r4
  mov r4, r12
  bl trim_controller_blocks_native
  cmp r0, #0
  bne 1f
  ldrb r1, [r5]
  b 2f
1: movs r1, #0
2: mov r12, r4
  /* The native BEQ at8642 consumes this exact selector comparison. */
  cmp r1, #0
  pop {r0,r2,r3,r4,r5,pc}

.section .text.trim_foreground,"ax",%progbits
.global trim_foreground
.thumb_func
trim_foreground:
  push {r4,r5,r6,lr}
  mov r4, r12
  bl trim_controller_poll
  bl trim_controller_blocks_native
  cmp r0, #0
  bne 3f
  mov r12, r4
  pop {r4,r5,r6}
  pop {r3}
  mov lr, r3
  ldr r3, =0x080051b5
  bx r3
3: mov r12, r4
  pop {r4,r5,r6}
  pop {r3}
  mov lr, r3
  /* Skip both the physical dispatcher and the R7 motion wrapper. */
  ldr r3, =0x0800868d
  bx r3
.balign 4
.ltorg

.section .text.trim_gpio,"ax",%progbits
.global trim_gpio
.thumb_func
trim_gpio:
  push {r3,r4,r5,r6,r7,lr}
  mov r4, r0
  mov r5, r1
  mov r6, r12
  bl trim_controller_inputs_blocked
  mov r7, r0
  mov r0, r4
  mov r1, r5
  ldr r3, =0x08008ca9
  blx r3
  cmp r7, #0
  beq 5f
  ldr r3, =0x48000000
  cmp r4, r3
  beq 4f
  ldr r3, =0x48000400
  cmp r4, r3
  bne 5f
  ldr r3, =0xc000
  b 6f
4: ldr r3, =0x1f00
6: orrs r2, r3
  movs r0, #0
  tst r2, r5
  beq 5f
  movs r0, #1
5: mov r12, r6
  mov r1, r5
  pop {r3,r4,r5,r6,r7,pc}
.balign 4
.ltorg
