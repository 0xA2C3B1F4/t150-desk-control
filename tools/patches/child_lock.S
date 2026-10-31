.syntax unified
.cpu cortex-m0
.thumb

.equ CL_RAM, 0x20000fc8

.section .gpio_hook,"ax",%progbits
.global child_lock_gpio_hook
.thumb_func
child_lock_gpio_hook:
    ldr r2, 1f
    bx r2
.balign 4
1:  .word child_lock_gpio

.section .boot_hook,"ax",%progbits
.global child_lock_boot_hook
.thumb_func
child_lock_boot_hook:
    bl child_lock_boot

.section .child_lock,"ax",%progbits
.global child_lock_boot
.thumb_func
child_lock_boot:
    push {r4,lr}
    ldr r4, =CL_RAM
    movs r0, #1
    strb r0, [r4]
    strb r0, [r4,#1]
    strb r0, [r4,#2]
    ldr r3, =0x08007621
    blx r3
    bl child_lock_load
    pop {r4,pc}

// Exact valid complementary records only. Return r0=locked, r1=fault.
// Stock read helper has no dependable error result; read both bytes twice.
.global child_lock_read_record
.thumb_func
child_lock_read_record:
    push {r4,r5,r6,lr}
    movs r0, #0x60
    ldr r6, =0x0800488d
    blx r6
    mov r4, r0
    movs r0, #0x61
    blx r6
    mov r5, r0
    movs r0, #0x60
    blx r6
    cmp r0, r4
    bne record_bad
    movs r0, #0x61
    blx r6
    cmp r0, r5
    bne record_bad
    cmp r4, #0xa5
    bne record_try_unlocked
    cmp r5, #0x5a
    bne record_bad
    movs r0, #1
    b record_valid
record_try_unlocked:
    cmp r4, #0x5a
    bne record_bad
    cmp r5, #0xa5
    bne record_bad
    movs r0, #0
record_valid:
    movs r1, #0
    pop {r4,r5,r6,pc}
record_bad:
    movs r0, #1
    movs r1, #1
    pop {r4,r5,r6,pc}

.global child_lock_load
.thumb_func
child_lock_load:
    push {r4,r5,r6,lr}
    mrs r5, PRIMASK
    cpsid i
    bl child_lock_read_record
    ldr r4, =CL_RAM
    strb r0, [r4]
    strb r1, [r4,#2]
    movs r0, #1
    strb r0, [r4,#1]
    bl child_lock_reset_scanners
    msr PRIMASK, r5
    pop {r4,r5,r6,pc}

// Return 1 while physical keys are hidden. After unlock, require every physical
// contact to be released before any physical key can enter the stock scanner.
.global child_lock_gate_refresh
.thumb_func
child_lock_gate_refresh:
    ldr r0, =CL_RAM
    ldrb r1, [r0]
    cmp r1, #0
    bne gate_suppressed
    ldrb r1, [r0,#1]
    cmp r1, #0
    beq gate_open
    ldr r1, =0x48000010
    ldrh r1, [r1]
    ldr r2, =0x1f00
    ands r1, r2
    cmp r1, r2
    bne gate_suppressed
    ldr r1, =0x48000410
    ldrh r1, [r1]
    ldr r2, =0xc000
    ands r1, r2
    cmp r1, r2
    bne gate_suppressed
    movs r1, #0
    strb r1, [r0,#1]
gate_open:
    movs r0, #0
    bx lr
gate_suppressed:
    movs r0, #1
    bx lr

// Tail replacement for stock helper4388. Change only PA8..12/PB14..15 IDR bits.
// Stock predicates test UART overrides before calling this helper.
.global child_lock_gpio
.thumb_func
child_lock_gpio:
    push {r4,r5,r6,lr}
    mov r4, r0
    mov r5, r1
    bl child_lock_gate_refresh
    ldrh r2, [r4,#0x10]
    cmp r0, #0
    beq gpio_test
    ldr r0, =0x48000000
    cmp r4, r0
    bne gpio_port_b
    ldr r0, =0x1f00
    b gpio_mask
gpio_port_b:
    ldr r0, =0x48000400
    cmp r4, r0
    bne gpio_test
    ldr r0, =0xc000
gpio_mask:
    orrs r2, r0
gpio_test:
    movs r0, #0
    tst r2, r5
    beq gpio_return
    movs r0, #1
gpio_return:
    mov r1, r5
    pop {r4,r5,r6,pc}

// Preserve callbacks at+0, clear scanner state/debounce/counter/events at+4..15.
// Clear the per-handler pending recall/click/store latches too. No release call.
.global child_lock_reset_scanners
.thumb_func
child_lock_reset_scanners:
    ldr r0, =0x20000a10
    movs r1, #8
    movs r2, #0
reset_scanner_loop:
    str r2, [r0,#4]
    str r2, [r0,#8]
    str r2, [r0,#12]
    adds r0, #16
    subs r1, #1
    bne reset_scanner_loop
    ldr r0, =0x20000184
    str r2, [r0]
    strb r2, [r0,#4]
    strb r2, [r0,#5]
    strb r2, [r0,#6]
    strb r2, [r0,#9]
    strb r2, [r0,#10]
    bx lr

// r0=value,r1=EEPROMaddress. Delay uses a polled hardware timer, not SysTick.
.thumb_func
write_byte_wait:
    push {r4,lr}
    ldr r3, =0x08004939
    blx r3
    movs r0, #3
    ldr r3, =0x08007b19
    blx r3
    pop {r4,pc}

// r0=address,r1=expected. Read twice; return1 only on two exact matches.
.thumb_func
verify_byte:
    push {r4,r5,r6,lr}
    mov r4, r0
    mov r5, r1
    ldr r6, =0x0800488d
    blx r6
    cmp r0, r5
    bne verify_bad
    mov r0, r4
    blx r6
    cmp r0, r5
    bne verify_bad
    movs r0, #1
    pop {r4,r5,r6,pc}
verify_bad:
    movs r0, #0
    pop {r4,r5,r6,pc}

.global child_lock_apply
.thumb_func
child_lock_apply:
    push {r3,r4,r5,r6,r7,lr}
    mov r4, r0
    mrs r7, PRIMASK
    cpsid i
    // Idempotent state writes are wear-free, but recheck persistent state.
    bl child_lock_read_record
    cmp r1, #0
    bne apply_change
    cmp r0, r4
    bne apply_change
    ldr r6, =CL_RAM
    ldrb r1, [r6]
    cmp r1, r4
    bne apply_change
    movs r1, #0
    strb r1, [r6,#2]
    b apply_ok
apply_change:
    ldr r6, =CL_RAM
    movs r0, #1
    strb r0, [r6]
    strb r0, [r6,#1]
    strb r0, [r6,#2]
    bl child_lock_reset_scanners
    // Invalidate first, so arbitrary old/invalid contents cannot unlock early.
    movs r0, #0
    movs r1, #0x60
    bl write_byte_wait
    movs r0, #0x60
    movs r1, #0
    bl verify_byte
    cmp r0, #1
    bne apply_failed
    movs r5, #0x5a
    cmp r4, #1
    beq apply_write_second
    movs r5, #0xa5
apply_write_second:
    mov r0, r5
    movs r1, #0x61
    bl write_byte_wait
    movs r0, #0x61
    mov r1, r5
    bl verify_byte
    cmp r0, #1
    bne apply_failed
    movs r0, #0x60
    movs r1, #0
    bl verify_byte
    cmp r0, #1
    bne apply_failed
    movs r0, #0xff
    eors r0, r5
    movs r1, #0x60
    bl write_byte_wait
    bl child_lock_read_record
    cmp r1, #0
    bne apply_failed
    cmp r0, r4
    bne apply_failed
    strb r4, [r6]
    movs r0, #0
    strb r0, [r6,#2]
apply_ok:
    movs r0, #0
    b apply_return
apply_failed:
    movs r0, #3
apply_return:
    msr PRIMASK, r7
    pop {r3,r4,r5,r6,r7,pc}

.balign 4
.ltorg

// The parser already validated XOR/framing. Leave ordinary requests to DT02,
// including its flag/register-preserving stock-dispatch trampoline.
.global child_lock_dispatch
.thumb_func
child_lock_dispatch:
    push {r1,r3}
    mrs r1, APSR
    ldr r0, =0x200000e8
    ldr r2, [r0]
    ldrb r3, [r2]
    cmp r3, #0xd3
    beq lock_request
    msr APSR_nzcvq, r1
    pop {r1,r3}
    b direct_target_entry
lock_request:
    pop {r1,r3}
    sub sp, #16
    mov r4, r2
    ldr r0, =0x20000124
    ldrb r0, [r0]
    cmp r0, #6
    bne lock_return
    ldr r0, =0x200000f0
    ldrb r1, [r0,#4]
    ldrb r2, [r0,#5]
    subs r1, r1, r2
    subs r1, #1
    uxtb r1, r1
    cmp r1, #35
    blo lock_return
    ldr r0, =0x200000e0
    ldr r5, [r0]
    ldrb r0, [r4,#2]
    str r0, [sp]
    ldrb r0, [r4,#3]
    lsls r0, #8
    ldrb r1, [r4,#4]
    orrs r0, r1
    str r0, [sp,#4]
    movs r7, #1
    ldrb r0, [r4,#1]
    cmp r0, #1
    bne lock_reply
    ldr r0, [sp]
    cmp r0, #0
    beq lock_get
    cmp r0, #2
    bhi lock_reply
    // Snapshot busy/protection state and commit under one interrupt mask. The
    // nested apply routine restores this masked state before we restore caller.
    mrs r0, PRIMASK
    cpsid i
    str r0, [sp,#8]
    ldr r1, =0x20000000
    ldr r2, =busy_offsets
busy_loop:
    ldrh r0, [r2]
    adds r2, #2
    cmp r0, #0
    beq busy_halfword
    ldrb r0, [r1,r0]
    cmp r0, #0
    beq busy_loop
    b lock_busy
busy_halfword:
    ldr r0, =0x75a
    ldrh r0, [r1,r0]
    cmp r0, #0
    bne lock_busy
    // A scanner already processing a press/release can leave private handler
    // deadlines or pending actions. Change lock state only from idle scanners.
    ldr r0, =0x20000a10
    movs r1, #8
busy_scanner_loop:
    ldrb r2, [r0,#4]
    ldrh r3, [r0,#12]
    orrs r2, r3
    ldrb r3, [r0,#14]
    orrs r2, r3
    bne lock_busy
    adds r0, #16
    subs r1, #1
    bne busy_scanner_loop
.global child_lock_busy_snapshot_done
child_lock_busy_snapshot_done:
    ldr r0, [sp]
    // wire1=lock -> RAM1, wire2=unlock -> RAM0
    movs r1, #2
    subs r0, r1, r0
    bl child_lock_apply
    mov r7, r0
    b lock_restore_mask
lock_busy:
    movs r7, #2
lock_restore_mask:
    ldr r0, [sp,#8]
    msr PRIMASK, r0
    b lock_reply
lock_get:
    movs r7, #0
lock_reply:
    bl child_lock_gate_refresh
    movs r0, #0xfa
    strb r0, [r5]
    movs r0, #0xd4
    strb r0, [r5,#1]
    movs r0, #1
    strb r0, [r5,#2]
    ldr r0, [sp]
    strb r0, [r5,#3]
    strb r7, [r5,#4]
    ldr r1, =CL_RAM
    ldrb r0, [r1]
    strb r0, [r5,#5]
    ldrb r0, [r1,#1]
    strb r0, [r5,#6]
    ldrb r0, [r1,#2]
    strb r0, [r5,#7]
    movs r0, #0x43
    strb r0, [r5,#8]
    movs r0, #0x4c
    strb r0, [r5,#9]
    movs r0, #0x30
    strb r0, [r5,#10]
    movs r0, #0x31
    strb r0, [r5,#11]
    ldr r0, [sp,#4]
    lsrs r1, r0, #8
    strb r1, [r5,#12]
    strb r0, [r5,#13]
    ldr r4, =0x20000120
    ldrh r0, [r4]
    lsrs r1, r0, #8
    strb r1, [r5,#14]
    strb r0, [r5,#15]
    ldr r1, =0x200000e0
    movs r0, #15
    strb r0, [r1,#4]
    ldr r0, =0x200000f0
    ldr r3, =0x08003611
    blx r3
    ldrh r0, [r4]
    adds r0, #1
    strh r0, [r4]
lock_return:
    add sp, #16
    pop {r3,r4,r5,r6,r7,pc}

.balign 4
.ltorg
busy_offsets:
    .hword 0x88d,0x895,0x88c,0x754,0x7ad,0x7ae,0x7af,0x7b0
    .hword 0x7b1,0x7b2,0x7b3,0x7b4
    .hword 0x184,0x185,0x186,0x187,0x188,0x189,0x18a,0x18d
    .hword 0x88f,0x890,0x148,0x8b8,0x892,0x893,0x894,0x8b9
    .hword 0xaa0,0xaa1,0x18e,0x8ba,0
