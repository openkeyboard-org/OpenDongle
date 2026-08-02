/*
 * N23 controlled fatal-fault policy for CH592.
 *
 * Both handler bodies remain flash-resident, free of software sp/gp references,
 * call-free, and non-returning. (Hardware exception entry may still use the
 * initialized stack.) They publish a versioned retained record, then allow one
 * automatic software reset per RPOR/GRWSM epoch. R8_GLOB_RESET_KEEP survives
 * software, watchdog, and manual resets, so a repeated fatal event records a
 * REPEAT_FAILSTOP disposition and halts instead of creating a reset storm.
 *
 * Bytes 0x20005800..03 remain the startup sentinel. The record layout from
 * 0x20005804 onward is shared with platform_ch592.c through fault_record.h and
 * is linker-pinned by link.ld.
 */

#include "fault_record.h"

	.section .text, "ax", @progbits
	.align 1

	/* Saturating increment of a retained u32. t1 is the record base. */
	.macro FAULT_SAT_INC offset
	lw t4, \offset(t1)
	li t5, -1
	beq t4, t5, .Lsat_done\@
	addi t4, t4, 1
.Lsat_done\@:
	sw t4, \offset(t1)
	.endm

	/* Publish validity only after every payload store is globally visible. */
	.macro FAULT_PUBLISH
	li t2, CH592_FAULT_MAGIC_INVERSE
	sw t2, CH592_FAULT_OFF_MAGIC_INVERSE(t1)
	fence iorw, iorw
	li t2, CH592_FAULT_MAGIC
	sw t2, CH592_FAULT_OFF_MAGIC(t1)
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

	/* Full handler body is expanded twice so neither vector depends on a call,
	 * return address, stack, or shared out-of-range tail. */
	.macro CH592_FATAL_HANDLER name, kind
	.global \name
	.type \name, @function
\name:
	li t1, CH592_FAULT_RECORD_BASE
	li t6, 0

	/* ResetKeep is the epoch authority. RPOR/GRWSM can leave SRAM bytes intact
	 * while clearing the keeper. Early C boot changes a freshly-cleared value
	 * to ARMED; only ARMED or CONSUMED may therefore reuse a valid record. Any
	 * other keeper value forces initialization even if stale magic remains. */
	li t0, 0x40001047
	lbu t2, 0(t0)
	li t3, CH592_FAULT_RESET_KEEP_CONSUMED
	beq t2, t3, .Lvalidate_consumed_record\@
	li t3, CH592_FAULT_RESET_KEEP_ARMED
	bne t2, t3, .Linit_record\@
	j .Lvalidate_record\@

.Lvalidate_consumed_record\@:
	/* If an NMI interrupts C while it is reconciling a consumed record, the
	 * invalid magic below makes this handler rebuild a repeat-failstop record.
	 * Carry that loss-of-evidence fact through the common payload path. */
	li t6, CH592_FAULT_FLAG_REBUILT

.Lvalidate_record\@:
	/* Within an initialized epoch, reuse counters only from a valid record.
	 * This preserves boot_count=1 for a normal post-main first fault while the
	 * unknown-keeper path above remains deterministic before the C boot hook. */
	lw t2, CH592_FAULT_OFF_MAGIC(t1)
	li t3, CH592_FAULT_MAGIC
	bne t2, t3, .Linit_record\@
	lw t2, CH592_FAULT_OFF_MAGIC_INVERSE(t1)
	li t3, CH592_FAULT_MAGIC_INVERSE
	bne t2, t3, .Linit_record\@
	lbu t2, CH592_FAULT_OFF_VERSION(t1)
	li t3, CH592_FAULT_RECORD_VERSION
	bne t2, t3, .Linit_record\@

	/* Preserve provenance already attached to a complete valid record. */
	lbu t6, CH592_FAULT_OFF_FLAGS(t1)
	/* magic=0 makes an interrupted/partial update unambiguously invalid. The
	 * fence is deliberately immediate: no payload or counter may change while
	 * old validity is still observable. */
	sw zero, CH592_FAULT_OFF_MAGIC(t1)
	fence iorw, iorw
	j .Lrecord_ready\@

.Linit_record\@:
	/* Invalidate before clearing even the first counter. */
	sw zero, CH592_FAULT_OFF_MAGIC(t1)
	fence iorw, iorw
	sw zero, CH592_FAULT_OFF_EVENT_COUNT(t1)
	sw zero, CH592_FAULT_OFF_RESET_REQUEST_COUNT(t1)
	sw zero, CH592_FAULT_OFF_REPEAT_COUNT(t1)
	sw zero, CH592_FAULT_OFF_BOOT_COUNT(t1)

.Lrecord_ready\@:
	li t2, CH592_FAULT_RECORD_VERSION
	sb t2, CH592_FAULT_OFF_VERSION(t1)
	li t2, \kind
	sb t2, CH592_FAULT_OFF_KIND(t1)
	sb t6, CH592_FAULT_OFF_FLAGS(t1)
	csrr t2, mepc
	sw t2, CH592_FAULT_OFF_MEPC(t1)
	csrr t2, mcause
	sw t2, CH592_FAULT_OFF_MCAUSE(t1)
	csrr t2, mtval
	sw t2, CH592_FAULT_OFF_MTVAL(t1)
	li t0, 0x40001044       /* R8_RESET_STATUS */
	lbu t2, 0(t0)
	sb t2, CH592_FAULT_OFF_LAST_RESET_STATUS(t1)
	sb zero, CH592_FAULT_OFF_RESERVED_0(t1)
	sb zero, CH592_FAULT_OFF_RESERVED_1(t1)

	li t0, 0x40001047       /* R8_GLOB_RESET_KEEP */
	lbu t2, 0(t0)
	li t3, CH592_FAULT_RESET_KEEP_CONSUMED
	beq t2, t3, .Lrepeat_fault\@

	/* First fatal event in this power epoch: consume the reset budget before
	 * publishing the breadcrumb, then request one direct software reset. */
	sb t3, 0(t0)
	sb t3, CH592_FAULT_OFF_RESET_KEEP(t1)
	FAULT_SAT_INC CH592_FAULT_OFF_EVENT_COUNT
	FAULT_SAT_INC CH592_FAULT_OFF_RESET_REQUEST_COUNT
	li t2, CH592_FAULT_ACTION_RESET_REQUESTED
	sb t2, CH592_FAULT_OFF_ACTION(t1)
	FAULT_PUBLISH

	/* Issue the MCU-reset half of SYS_ResetExecute without calling SDK/ROM code.
	 * The vendor helper first resets the FlashROM controller through a ROM call,
	 * but that call is not stack-safe in a corrupted fault context. IAP staging
	 * is already disarmed before writes, so an interrupted update stays
	 * unpromotable. Writing literal 1 here clears RB_WDOG_RST_EN and makes reset
	 * status report software reset. */
	li t0, 0x40001040       /* R8_SAFE_ACCESS_SIG */
	fence.i
	li t2, 0x57
	sb t2, 0(t0)
	li t2, 0xA8
	sb t2, 0(t0)
	fence.i
	li t2, 1
	sb t2, 6(t0)           /* R8_RST_WDOG_CTRL = RB_SOFTWARE_RESET */
	sb zero, 0(t0)          /* reached only if reset is not immediate */
	fence.i
.Lreset_fallback\@:
	j .Lreset_fallback\@

.Lrepeat_fault\@:
	/* Do not let an already-armed watchdog turn the fail-stop into another
	 * reset. This protected MMIO sequence is best-effort in a fault context. */
	FAULT_DISABLE_RESET_ESCALATION
	li t3, CH592_FAULT_RESET_KEEP_CONSUMED
	sb t3, CH592_FAULT_OFF_RESET_KEEP(t1)
	FAULT_SAT_INC CH592_FAULT_OFF_EVENT_COUNT
	FAULT_SAT_INC CH592_FAULT_OFF_REPEAT_COUNT
	li t2, CH592_FAULT_ACTION_REPEAT_FAILSTOP
	sb t2, CH592_FAULT_OFF_ACTION(t1)
	FAULT_PUBLISH
.Lfail_stop\@:
	j .Lfail_stop\@
	.size \name, .-\name
	.endm

	CH592_FATAL_HANDLER HardFault_Handler, CH592_FAULT_KIND_HARDFAULT
	CH592_FATAL_HANDLER NMI_Handler, CH592_FAULT_KIND_NMI
