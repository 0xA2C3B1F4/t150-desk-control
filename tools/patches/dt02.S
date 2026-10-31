.syntax unified
.cpu cortex-m0
.thumb

.section .hook,"ax",%progbits
.global direct_target_hook
.thumb_func
direct_target_hook:
#ifdef CHILD_LOCK
    bl child_lock_dispatch
#else
    bl direct_target_entry
#endif

.section .cave,"ax",%progbits
.global direct_target_entry
.thumb_func
direct_target_entry:
    push {r1,r3}
    mrs r1, APSR
    ldr r0, =0x200000e8
    ldr r2, [r0]
    ldrb r3, [r2]
    cmp r3, #0xd1
    beq private_request
    msr APSR_nzcvq, r1
    pop {r1,r3}
    ldr r0, [sp,#20]
    mov lr, r0
    ldr r0, =0x08007bb3
    push {r0}
    ldr r0, =0x200000e8
    pop {pc}

private_request:
    pop {r1,r3}
    sub sp, #32
    mov r4, r2
    ldr r6, =0x20000000
    ldr r0, =0x124
    ldrb r0, [r6,r0]
    cmp r0, #8
    beq length_ok
    b private_return
length_ok:
    ldr r0, =0x200000f0
    ldrb r1, [r0,#4]
    ldrb r2, [r0,#5]
    subs r1, r1, r2
    subs r1, #1
    uxtb r1, r1
    cmp r1, #77
    bhs queue_ok
    b private_return
queue_ok:
    ldr r0, =0x200000e0
    ldr r5, [r0]
    ldrb r0, [r4,#5]
    lsls r0, #8
    ldrb r1, [r4,#6]
    orrs r0, r1
    str r0, [sp,#0]
    ldrb r0, [r4,#3]
    lsls r0, #8
    ldrb r1, [r4,#4]
    orrs r0, r1
    str r0, [sp,#4]
    ldrb r0, [r4,#2]
    str r0, [sp,#8]
    movs r0, #0
    str r0, [sp,#12]
    mrs r0, PRIMASK
    cpsid i
    str r0, [sp,#24]
    ldr r0, =0x76a
    ldrh r0, [r6,r0]
    str r0, [sp,#16]
    ldr r0, =0xa9c
    ldr r0, [r6,r0]
    str r0, [sp,#20]
    movs r7, #0

    ldr r3, =busy_bytes
    bl any_nonzero
    cmp r0, #0
    bne busy
    ldr r0, =0x75a
    ldrh r0, [r6,r0]
    cmp r0, #0
    beq check_inhibit
busy:
    movs r7, #1
check_inhibit:
    ldr r3, =inhibit_bytes
    bl any_nonzero
    lsls r0, #1
    orrs r7, r0
    // Stock startup qualification is 0 or 1, not a blanket motion inhibit.
    ldr r0, =0x891
    ldrb r0, [r6,r0]
    cmp r0, #1
    bls check_reset
    movs r0, #2
    orrs r7, r0
check_reset:
    ldr r0, =0x754
    ldrb r0, [r6,r0]
    cmp r0, #0
    beq check_keys
    movs r0, #32
    orrs r7, r0
check_keys:
    ldr r3, =key_bytes
    bl any_nonzero
    cmp r0, #0
    bne keys_busy
    ldr r0, =0x20000a10
    movs r1, #8
scanner_loop:
    ldrb r2, [r0,#4]
    ldrh r3, [r0,#12]
    orrs r2, r3
    ldrb r3, [r0,#14]
    orrs r2, r3
    bne keys_busy
    adds r0, #16
    subs r1, #1
    bne scanner_loop
#ifdef CHILD_LOCK
    // Physical keys are intentionally hidden by CL01; UART/scanner guards above
    // still apply. Keep the ordinary raw-IDR exclusion whenever keys are live.
    bl child_lock_gate_refresh
    cmp r0, #0
    bne check_profile
#endif
    ldr r0, =0x48000010
    ldrh r0, [r0]
    ldr r1, =0x1f00
    ands r0, r1
    cmp r0, r1
    bne keys_busy
    ldr r0, =0x48000410
    ldrh r0, [r0]
    ldr r1, =0xc000
    ands r0, r1
    cmp r0, r1
    beq check_profile
keys_busy:
    movs r0, #4
    orrs r7, r0
check_profile:
    ldr r3, =profile_words
profile_loop:
    ldrh r0, [r3]
    cmp r0, #0
    beq check_range
    ldrh r1, [r3,#2]
    ldr r2, [r6,r0]
    cmp r2, r1
    bne profile_bad
    adds r3, #4
    b profile_loop
profile_bad:
    movs r0, #8
    orrs r7, r0
check_range:
    ldr r1, =747
    ldr r0, [sp,#16]
    cmp r0, #6
    blo range_bad
    cmp r0, r1
    bhi range_bad
    ldr r0, [sp,#8]
    cmp r0, #1
    bne check_request
    ldr r0, [sp,#0]
    cmp r0, #6
    blo range_bad
    cmp r0, r1
    bls check_request
range_bad:
    movs r0, #16
    orrs r7, r0
check_request:
    ldrb r0, [r4,#1]
    cmp r0, #1
    bne bad_request
    ldr r0, [sp,#8]
    cmp r0, #1
    beq set_request
    cmp r0, #0
    bne bad_request
    ldr r0, [sp,#0]
    cmp r0, #0
    bne bad_request
    b reply
bad_request:
    movs r0, #1
    b rejected
set_request:
    movs r0, #1
    tst r7, r0
    beq no_busy
    movs r0, #2
    b rejected
no_busy:
    movs r0, #34
    tst r7, r0
    beq no_inhibit
    movs r0, #3
    b rejected
no_inhibit:
    movs r0, #4
    tst r7, r0
    beq no_keys
    movs r0, #6
    b rejected
no_keys:
    movs r0, #8
    tst r7, r0
    beq no_profile
    movs r0, #4
    b rejected
no_profile:
    movs r0, #16
    tst r7, r0
    beq commit
    movs r0, #5
rejected:
    str r0, [sp,#12]
    b reply
commit:
    // Mirror stock preset recall only after every independent guard passed.
    // Actual stock helper 4396 performs STRH #0x80 at GPIOF BRR +0x28.
    ldr r1, =0x891
    ldrb r0, [r6,r1]
    cmp r0, #1
    bne calculate_target
    movs r0, #0
    strb r0, [r6,r1]
    ldr r0, =0x48001400
    movs r1, #0x80
    ldr r3, =0x08004397
    blx r3
calculate_target:
    ldr r0, [sp,#0]
    ldr r1, [sp,#16]
    adds r2, r1, #4
    cmp r0, r2
    ble try_down
    adds r0, #7
    b clamp
try_down:
    subs r1, #4
    cmp r0, r1
    bge clamp
    subs r0, #6
clamp:
    cmp r0, #6
    bge clamp_high
    movs r0, #6
clamp_high:
    ldr r1, =747
    cmp r0, r1
    ble commit_target
    mov r0, r1
commit_target:
    str r0, [sp,#20]
    ldr r1, =0xa9c
    str r0, [r6,r1]
    ldr r1, =0x890
    movs r0, #0
    strb r0, [r6,r1]
    ldr r1, =0x88d
    movs r0, #1
    strb r0, [r6,r1]
    ldr r3, =0x08006035
    blx r3
    ldr r0, =0x200009ec
    ldr r3, =0x0800605d
    blx r3

reply:
    ldr r0, =0x88d
    ldrb r1, [r6,r0]
    ldr r0, =0x895
    ldrb r0, [r6,r0]
    lsls r0, #8
    orrs r0, r1
    str r0, [sp,#28]
    ldr r0, [sp,#24]
    msr PRIMASK, r0
    mov r1, r5
    movs r0, #0xfa
    strb r0, [r1]
    adds r1, #1
    movs r0, #0xd2
    strb r0, [r1]
    movs r0, #1
    strb r0, [r1,#1]
    movs r0, #3
    strb r0, [r1,#2]
    ldr r0, [sp,#8]
    strb r0, [r1,#3]
    ldr r0, [sp,#12]
    strb r0, [r1,#4]
    movs r0, #0x44
    strb r0, [r1,#5]
    movs r0, #0x54
    strb r0, [r1,#6]
    movs r0, #0x30
    strb r0, [r1,#7]
    movs r0, #0x32
    strb r0, [r1,#8]
    adds r1, #9
    ldr r0, [sp,#4]
    bl put16
    ldr r0, [sp,#0]
    bl put16
    ldr r0, [sp,#16]
    bl put16
    ldr r0, [sp,#20]
    bl put32
    ldr r0, =0x160
    ldr r0, [r6,r0]
    bl put32
    ldr r0, =0x15c
    ldr r0, [r6,r0]
    bl put32
    ldr r0, =0x850
    ldr r0, [r6,r0]
    bl put16
    ldr r0, =0x854
    ldr r0, [r6,r0]
    bl put16
    ldr r0, [sp,#28]
    strb r0, [r1]
    lsrs r0, #8
    strb r0, [r1,#1]
    adds r1, #2
    mov r0, r7
    bl put16
    ldr r0, =0x120
    ldrh r0, [r6,r0]
    bl put16
    ldr r1, =0x200000e0
    movs r0, #37
    strb r0, [r1,#4]
    ldr r0, =0x200000f0
    ldr r3, =0x08003611
    blx r3
    ldr r1, =0x20000120
    ldrh r0, [r1]
    adds r0, #1
    strh r0, [r1]
private_return:
    add sp, #32
    pop {r3,r4,r5,r6,r7,pc}

.thumb_func
any_nonzero:
    ldrh r0, [r3]
    adds r3, #2
    cmp r0, #0
    beq zero_return
    ldrb r0, [r6,r0]
    cmp r0, #0
    beq any_nonzero
    movs r0, #1
zero_return:
    bx lr
.thumb_func
put16:
    lsrs r2, r0, #8
    strb r2, [r1]
    strb r0, [r1,#1]
    adds r1, #2
    bx lr
.thumb_func
put32:
    lsrs r2, r0, #24
    strb r2, [r1]
    lsrs r2, r0, #16
    strb r2, [r1,#1]
    lsrs r2, r0, #8
    strb r2, [r1,#2]
    strb r0, [r1,#3]
    adds r1, #4
    bx lr

.balign 4
.ltorg
busy_bytes:
    .hword 0x88d,0x895,0x88c,0
inhibit_bytes:
    .hword 0x88f,0x890,0x148,0x8b8,0x892,0x893,0x894,0x8b9,0xaa0,0xaa1,0x18e,0x8ba,0
key_bytes:
    .hword 0x7ad,0x7ae,0x7af,0x7b0,0x7b1,0x7b2,0x7b3,0x7b4
    .hword 0x184,0x185,0x186,0x187,0x188,0x189,0x18a,0x18d,0
profile_words:
    .hword 0x850,24,0x854,39,0x160,6,0x15c,747,0,0
