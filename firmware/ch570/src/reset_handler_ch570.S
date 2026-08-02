/*
 * CH570 N23 reset owner.
 *
 * The vendor startup still owns _start and the runtime vector image. This
 * strong handle_reset replaces only its weak reset body so NMI/HardFault are
 * installed immediately after gp/sp setup, before the normal .highcode copy.
 * Four retained startup bytes record phase and pristine reset status.
 */

	.section .handle_reset_n23, "ax", @progbits
	.global handle_reset
	.type handle_reset, @function
	.align 1

	.macro FAULT_KEEP_ADDR reg
	.option push
	.option norelax
	la \reg, _fault_keep_start
	.option pop
	.endm

	.macro STARTUP_MARKER value
	li t0, \value
	FAULT_KEEP_ADDR t1
	sb t0, 0(t1)
	.endm

handle_reset:
	/* Capture reset cause before any peripheral/clock initialization. */
	li t1, 0x40001044
	lbu t0, 0(t1)
	FAULT_KEEP_ADDR t1
	sb t0, 1(t1)
	STARTUP_MARKER 0xC0

	.option push
	.option norelax
	la gp, __global_pointer$
	la sp, _eusrstack
	.option pop

	/* Own both fatal vectors before copying the rest of .highcode. */
	li t1, 0x20000000
	.option push
	.option norelax
	la t0, NMI_Handler
	.option pop
	sw t0, 8(t1)
	.option push
	.option norelax
	la t0, HardFault_Handler
	.option pop
	sw t0, 12(t1)
	fence rw, rw
	li t0, 0x20000003
	csrw mtvec, t0
	STARTUP_MARKER 0xC1

	/* Load .highcode (including the final vector image) from flash to RAM. */
	la a0, _highcode_lma
	la a1, _highcode_vma_start
	la a2, _highcode_vma_end
	bgeu a1, a2, 2f
1:
	lw t0, 0(a0)
	sw t0, 0(a1)
	addi a0, a0, 4
	addi a1, a1, 4
	bltu a1, a2, 1b
2:
	STARTUP_MARKER 0xC2

	/* Load initialized data. */
	la a0, _data_lma
	la a1, _data_vma
	la a2, _edata
	bgeu a1, a2, 2f
1:
	lw t0, 0(a0)
	sw t0, 0(a1)
	addi a0, a0, 4
	addi a1, a1, 4
	bltu a1, a2, 1b
2:
	STARTUP_MARKER 0xC3

	/* Clear BSS; the linker-owned fault block begins after _ebss. */
	la a0, _sbss
	la a1, _ebss
	bgeu a0, a1, 2f
1:
	sw zero, 0(a0)
	addi a0, a0, 4
	bltu a0, a1, 1b
2:
	STARTUP_MARKER 0xC4

	/* Preserve the vendor CH57x execution-mode and HPE setup. */
	li t0, 0x25
	csrw 0xbc0, t0
	li t0, 0x3
	csrw 0x804, t0
	li t0, 0x1
	csrw 0xbc1, t0
	li t0, 0x88
	csrw mstatus, t0
	la t0, _vector_base
	ori t0, t0, 3
	csrw mtvec, t0
	la t0, main
	csrw mepc, t0
	STARTUP_MARKER 0xC5
	mret

	.size handle_reset, .-handle_reset
