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
  push {r0,r1,r2,r3,r4,lr}
  mrs r4, APSR
  bl tb01_native_priority
  cmp r0, #0
  bne 2f
  msr APSR_nzcvq, r4
  pop {r0,r1,r2,r3,r4}
  pop {r3}
  mov lr, r3
  push {r3,r4,r5,r6,r7,lr}
  ldr r5, =0x200008a8
  mov r4, r0
  mov r7, r1
  ldr r3, =0x08003411
  bx r3
2:
  msr APSR_nzcvq, r4
  pop {r0,r1,r2,r3,r4,pc}
.ltorg
.section .hook_buzzer_timer,"ax",%progbits
  ldr r3, 3f
  bx r3
3: .word buzzer_native_timer
.section .text.buzzer_native_timer,"ax",%progbits
.global buzzer_native_timer
.thumb_func
buzzer_native_timer:
  push {r0,r1,r2,r3,r4,lr}
  bl buzzer_timer_replaced
  pop {r0,r1,r2,r3,r4}
  pop {r3}
  mov lr, r3
  /* Replay the overwritten prologue and division before continuing. */
  push {r4,r5,r6,lr}
  movs r1, #60
  ldr r3, =0x080028f5
  blx r3
  ldr r3, =0x080067f5
  bx r3
.ltorg
