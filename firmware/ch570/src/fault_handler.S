/*
 * N23 controlled fatal-fault policy for CH570.
 *
 * Both handlers are strong, flash-resident, call-free, non-returning, and use
 * neither software sp nor gp references. They publish a transactional retained
 * record and permit one automatic software reset per reset-keeper epoch. A
 * repeated fatal event records REPEAT_FAILSTOP and halts instead of forming a
 * reset/re-enumeration storm.
 */

#include "fault_record.h"

	.section .text.ch570_fault_handlers, "ax", @progbits
	.align 1

	.macro FAULT_SAT_INC offset
	lw t4, \offset(t1)
	li t5, -1
	beq t4, t5, .Lsat_done\@
	addi t4, t4, 1
.Lsat_done\@:
	sw t4, \offset(t1)
	.endm

	.macro FAULT_PUBLISH
	li t2, CH570_FAULT_MAGIC_INVERSE
	sw t2, CH570_FAULT_OFF_MAGIC_INVERSE(t1)
	fence iorw, iorw
	li t2, CH570_FAULT_MAGIC
	sw t2, CH570_FAULT_OFF_MAGIC(t1)
	fence iorw, iorw
	.endm

	/* WCH protected write: disable watchdog reset/interrupt escalation. */
	.macro FAULT_DISABLE_RESET_ESCALATION
	li t0, 0x40001040
	fence.i
	li t2, 0x57
	sb t2, 0(t0)
	li t2, 0xA8
	sb t2, 0(t0)
	fence.i
	sb zero, 6(t0)       /* R8_RST_WDOG_CTRL = 0 */
	sb zero, 0(t0)       /* leave safe-access mode */
	fence.i
	.endm

	.macro CH570_FATAL_HANDLER name, kind
	.global \name
	.type \name, @function
\name:
	/* Force a PC-relative symbol materialization; linker relaxation must never
	 * turn the retained-record base into a gp-relative access. */
	.option push
	.option norelax
	la t1, ch570_fault_record
	.option pop
	li t6, 0

	/* ResetKeep is the epoch authority. Unknown means a freshly cleared epoch
	 * or a fault before the C boot hook; stale SRAM is not trusted in that case. */
	li t0, 0x40001047
	lbu t2, 0(t0)
	li t3, CH570_FAULT_RESET_KEEP_CONSUMED
	beq t2, t3, .Lvalidate_consumed_record\@
	li t3, CH570_FAULT_RESET_KEEP_ARMED
	bne t2, t3, .Linit_record\@
	j .Lvalidate_record\@

.Lvalidate_consumed_record\@:
	li t6, CH570_FAULT_FLAG_REBUILT

.Lvalidate_record\@:
	lw t2, CH570_FAULT_OFF_MAGIC(t1)
	li t3, CH570_FAULT_MAGIC
	bne t2, t3, .Linit_record\@
	lw t2, CH570_FAULT_OFF_MAGIC_INVERSE(t1)
	li t3, CH570_FAULT_MAGIC_INVERSE
	bne t2, t3, .Linit_record\@
	lbu t2, CH570_FAULT_OFF_VERSION(t1)
	li t3, CH570_FAULT_RECORD_VERSION
	bne t2, t3, .Linit_record\@

	/* Preserve provenance, then invalidate before changing any payload. */
	lbu t6, CH570_FAULT_OFF_FLAGS(t1)
	sw zero, CH570_FAULT_OFF_MAGIC(t1)
	fence iorw, iorw
	j .Lrecord_ready\@

.Linit_record\@:
	sw zero, CH570_FAULT_OFF_MAGIC(t1)
	fence iorw, iorw
	sw zero, CH570_FAULT_OFF_EVENT_COUNT(t1)
	sw zero, CH570_FAULT_OFF_RESET_REQUEST_COUNT(t1)
	sw zero, CH570_FAULT_OFF_REPEAT_COUNT(t1)
	sw zero, CH570_FAULT_OFF_BOOT_COUNT(t1)

.Lrecord_ready\@:
	li t2, CH570_FAULT_RECORD_VERSION
	sb t2, CH570_FAULT_OFF_VERSION(t1)
	li t2, \kind
	sb t2, CH570_FAULT_OFF_KIND(t1)
	sb t6, CH570_FAULT_OFF_FLAGS(t1)
	csrr t2, mepc
	sw t2, CH570_FAULT_OFF_MEPC(t1)
	csrr t2, mcause
	sw t2, CH570_FAULT_OFF_MCAUSE(t1)
	csrr t2, mtval
	sw t2, CH570_FAULT_OFF_MTVAL(t1)
	li t0, 0x40001044       /* R8_RESET_STATUS */
	lbu t2, 0(t0)
	sb t2, CH570_FAULT_OFF_LAST_RESET_STATUS(t1)
	sb zero, CH570_FAULT_OFF_RESERVED_0(t1)
	sb zero, CH570_FAULT_OFF_RESERVED_1(t1)

	li t0, 0x40001047       /* R8_GLOB_RESET_KEEP */
	lbu t2, 0(t0)
	li t3, CH570_FAULT_RESET_KEEP_CONSUMED
	beq t2, t3, .Lrepeat_fault\@

	/* Consume the sole reset budget before publishing/requesting the reset. */
	sb t3, 0(t0)
	sb t3, CH570_FAULT_OFF_RESET_KEEP(t1)
	FAULT_SAT_INC CH570_FAULT_OFF_EVENT_COUNT
	FAULT_SAT_INC CH570_FAULT_OFF_RESET_REQUEST_COUNT
	li t2, CH570_FAULT_ACTION_RESET_REQUESTED
	sb t2, CH570_FAULT_OFF_ACTION(t1)
	FAULT_PUBLISH

	/* Direct MCU reset: omit the SDK/ROM helper because it uses the stack. A
	 * literal 1 also clears watchdog-reset enable, preserving SW reset status. */
	li t0, 0x40001040
	fence.i
	li t2, 0x57
	sb t2, 0(t0)
	li t2, 0xA8
	sb t2, 0(t0)
	fence.i
	li t2, 1
	sb t2, 6(t0)
	sb zero, 0(t0)
	fence.i
.Lreset_fallback\@:
	j .Lreset_fallback\@

.Lrepeat_fault\@:
	FAULT_DISABLE_RESET_ESCALATION
	li t3, CH570_FAULT_RESET_KEEP_CONSUMED
	sb t3, CH570_FAULT_OFF_RESET_KEEP(t1)
	FAULT_SAT_INC CH570_FAULT_OFF_EVENT_COUNT
	FAULT_SAT_INC CH570_FAULT_OFF_REPEAT_COUNT
	li t2, CH570_FAULT_ACTION_REPEAT_FAILSTOP
	sb t2, CH570_FAULT_OFF_ACTION(t1)
	FAULT_PUBLISH
.Lfail_stop\@:
	j .Lfail_stop\@
	.size \name, .-\name
	.endm

	CH570_FATAL_HANDLER HardFault_Handler, CH570_FAULT_KIND_HARDFAULT
	CH570_FATAL_HANDLER NMI_Handler, CH570_FAULT_KIND_NMI
