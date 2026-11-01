.syntax unified
.cpu cortex-m0
.thumb

.section .rs_vector,"a",%progbits
.word rs_systick
.section .rs_boot_hook,"ax",%progbits
bl rs_boot
.section .rs_uart_hook,"ax",%progbits
bl rs_dispatch
.section .rs_parser_hook,"ax",%progbits
ldr r3, 1f
bx r3
.balign 4
1: .word rs_parser
.section .rs_rx_reset_hook,"ax",%progbits
bl rs_rx_reset
.section .rs_main_hook,"ax",%progbits
bl rs_main
.section .rs_speed_hook,"ax",%progbits
bl rs_speed
.section .rs_output_hook,"ax",%progbits
ldr r3, 1f
bx r3
.balign 4
1: .word rs_output
.section .rs_complete_hook,"ax",%progbits
bl rs_complete

.section .reset_speed,"ax",%progbits
.global rs_boot
.thumb_func
rs_boot:
    push {r4,lr}
    bl rs_init
    bl rs_parser_init
    bl child_lock_boot
    pop {r4,pc}

.global rs_rx_reset
.thumb_func
rs_rx_reset:
    // Preserve the original RX initializer's return registers and flags.
    // This call is shared by cold boot and runtime UART reinitialization.
    push {r4,lr}
    bl rs_stock_rx_init
    push {r0,r1,r2,r3}
    mrs r0, APSR
    push {r0,r4}
    bl rs_parser_init
    pop {r0,r4}
    msr APSR_nzcvq, r0
    pop {r0,r1,r2,r3}
    pop {r4,pc}

.global rs_dispatch
.thumb_func
rs_dispatch:
    // The stock dispatcher already pushed r3-r7,lr. Pass-through must restore
    // its incoming volatile registers and APSR before entering CL01.
    push {r0,r1,r2,r3}
    mrs r0, APSR
    push {r0,lr}
    bl rs_route
    cmp r0, #0
    bne 1f
    pop {r0,r1}
    msr APSR_nzcvq, r0
    mov lr, r1
    pop {r0,r1,r2,r3}
    b child_lock_dispatch
1:  add sp, #24
    pop {r3,r4,r5,r6,r7,pc}

.global rs_systick
.thumb_func
rs_systick:
    push {r4,lr}
    ldr r3, =0x080073c1
    blx r3
    bl rs_watchdog
    pop {r4,pc}

.global rs_main
.thumb_func
rs_main:
    push {r4,lr}
    bl rs_before_main
    cmp r0, #0
    bne 2f
    ldr r3, =0x08005a59
    blx r3
    bl rs_after_main
2:  pop {r4,pc}

.global rs_complete
.thumb_func
rs_complete:
    // This exact stock branch has already cut output and selected mode0.
    // Retain the original position/state EEPROM save before reporting success.
    push {r4,lr}
    ldr r3, =0x080058d1
    blx r3
    bl rs_completed
    pop {r4,pc}

.global rs_speed
.thumb_func
rs_speed:
    // Replaces LDR r1,[r4,#32]; LDR r0,[r4,#4]. Preserve every other register
    // and flags. LR is already saved by the stock position-controller frame.
    push {r2,r3,r4,r5,r6,lr}
    mrs r5, APSR
    bl rs_cap
    msr APSR_nzcvq, r5
    pop {r2,r3,r4,r5,r6}
    pop {r0}
    mov lr, r0
    ldr r1, [r4,#32]
    ldr r0, [r4,#4]
    bx lr
.balign 4
.ltorg
