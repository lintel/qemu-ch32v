/* Standalone QingKe V4 interrupt acknowledgement/return regression. */
.option norvc
.section .text
.global _start
_start:
    li sp, 0x20007ff0
    csrw 0x804, zero
    la t0, vectors
    ori t0, t0, 3
    csrw mtvec, t0
    li s0, 0x20000000
    li t0, 0xe000e104
    li t1, 0x00201000
    sw t1, 0(t0)
    li t1, 0x2000
    sw t1, 4(t0)
    li t0, 0x40000000
    li t1, 1
    sw t1, 12(t0)
    li t0, 0x20001000
    li t1, 0x80000000
    sw t1, 0(t0)
    li t1, 0x47ff
    sw t1, 4(t0)
    li t1, 0x20002000
    sw t1, 8(t0)
    sw t0, 12(t0)
    li t1, 0x4002900c
    sw t0, 0(t1)
    li t0, 0x10040
    sw t0, 16(t1)
    li t0, 2
    sw t0, 12(t1)
    li t0, 0x1234
    sw t0, 28(s0)
    li t0, 0x1888
    csrw mstatus, t0
1:  j 1b

.balign 1024
vectors:
.rept 44
    .word fault
.endr
    .word timer_irq
.rept 8
    .word fault
.endr
    .word uart_irq
.rept 23
    .word fault
.endr
    .word ethernet_irq
.rept 178
    .word fault
.endr

timer_irq:
    lw t0, 4(s0)
    addi t0, t0, 1
    sw t0, 4(s0)
    li t0, 0x4001380c
    li t1, 0x2088
    sw t1, 0(t0)
    li t0, 0x40000010
    sw zero, 0(t0)
    li t0, 0xe000e304
    lw t1, 0(t0)
    sw t1, 8(s0)
1:  lw t0, 0(s0)
    beqz t0, 1b
    mret

uart_irq:
    li t0, 0x4001380c
    li t1, 0x2008
    sw t1, 0(t0)
    lw t0, 12(s0)
    addi t0, t0, 1
    sw t0, 12(s0)
    mret

ethernet_irq:
    li t0, 0x40029014
    li t1, 0x10040
    sw t1, 0(t0)
    li t0, 0xe000e308
    lw t1, 0(t0)
    sw t1, 20(s0)
    lw t0, 16(s0)
    addi t0, t0, 1
    sw t0, 16(s0)
1:  lw t0, 0(s0)
    li t1, 2
    bne t0, t1, 1b
    mret

fault:
    li t0, 0xdeaddead
    sw t0, 32(s0)
1:  j 1b
