/*
 * Phased CH592F startup — fork of vendor startup_CH592.S with SRAM
 * markers at each reset phase. Sentinel byte at 0x20005800 advances
 * through 0xC0 .. 0xC5 as reset progresses and STOPS there. A value below 0xC5
 * pinpoints a wedge earlier in startup; 0xC5 means startup reached the main
 * handoff (not proof main ran). Read without depending on GDB
 * attach/halt behavior. (The 0xA0+ "main writes" scheme below was inherited
 * from the keyboard project fw-ch592f and is NOT used by this dongle — the
 * dongle's main() writes no sentinel; boot-entry evidence comes from the
 * fault page's ch592_boot_reset_status, populated by dongle_fault_boot() at
 * the top of main — CODEREVIEW N23.)
 *
 * Marker map:
 *   0xC0 — after reset-status capture (no stack, no gp yet)
 *   0xC1 — after gp + sp setup and early N23 fatal-vector install
 *   0xC2 — after .highcode flash→SRAM copy
 *   0xC3 — after .data flash→SRAM copy
 *   0xC4 — after .bss zero-clear
 *   0xC5 — after CSR/mtvec setup, immediately before mret to main
 *          (last sentinel this dongle writes; 0xC5 persisting = startup
 *          reached the main handoff, NOT proof main() ran)
 *
 * This remains a narrow fork of
 * ~/Development/Personal/ch592/EVT/EXAM/SRC/Startup/startup_CH592.S: the
 * marker stores and the early N23 NMI/HardFault vector install are the only
 * deliberate sequencing additions.
 */

	.section	.init,"ax",@progbits
	.global	_start
	.align	1
_start:
	j	handle_reset

    .section    .vector,"ax",@progbits
    .align  1
_vector_base:
    .option norvc;

    .word   0
    .word   0
    .word   NMI_Handler                 /* NMI Handler */
    .word   HardFault_Handler           /* Hard Fault Handler */
    .word   0xF5F9BDA9
    .word   Ecall_M_Mode_Handler        /* 5 */
    .word   0
    .word   0
    .word   Ecall_U_Mode_Handler		/* 8 */
    .word   Break_Point_Handler			/* 9 */
    .word   0
    .word   0
    .word   SysTick_Handler            /* SysTick Handler */
    .word   0
    .word   SW_Handler                 /* SW Handler */
    .word   0
    /* External Interrupts */
    .word   TMR0_IRQHandler            /* 0:  TMR0 */
    .word   GPIOA_IRQHandler           /* GPIOA */
    .word   GPIOB_IRQHandler           /* GPIOB */
    .word   SPI0_IRQHandler            /* SPI0 */
    .word   BB_IRQHandler              /* BLEB */
    .word   LLE_IRQHandler             /* BLEL */
    .word   USB_IRQHandler             /* USB */
    .word   0
    .word   TMR1_IRQHandler            /* TMR1 */
    .word   TMR2_IRQHandler            /* TMR2 */
    .word   UART0_IRQHandler           /* UART0 */
    .word   UART1_IRQHandler           /* UART1 */
    .word   RTC_IRQHandler             /* RTC */
    .word   ADC_IRQHandler             /* ADC */
    .word   I2C_IRQHandler 			   /* I2C */
    .word   PWMX_IRQHandler            /* PWMX */
    .word   TMR3_IRQHandler            /* TMR3 */
    .word   UART2_IRQHandler           /* UART2 */
    .word   UART3_IRQHandler           /* UART3 */
    .word   WDOG_BAT_IRQHandler        /* WDOG_BAT */

    .option rvc;

    .section    .vector_handler, "ax", @progbits
    .weak   NMI_Handler
    .weak   HardFault_Handler
    .weak   Ecall_M_Mode_Handler
    .weak   Ecall_U_Mode_Handler
    .weak   Break_Point_Handler
    .weak   SysTick_Handler
    .weak   SW_Handler
    .weak   TMR0_IRQHandler
    .weak   GPIOA_IRQHandler
    .weak   GPIOB_IRQHandler
    .weak   SPI0_IRQHandler
    .weak   BB_IRQHandler
    .weak   LLE_IRQHandler
    .weak   USB_IRQHandler
    .weak   TMR1_IRQHandler
    .weak   TMR2_IRQHandler
    .weak   UART0_IRQHandler
    .weak   UART1_IRQHandler
    .weak   RTC_IRQHandler
    .weak   ADC_IRQHandler
    .weak   I2C_IRQHandler
    .weak   PWMX_IRQHandler
    .weak   TMR3_IRQHandler
    .weak   UART2_IRQHandler
    .weak   UART3_IRQHandler
    .weak   WDOG_BAT_IRQHandler

NMI_Handler:
HardFault_Handler:
Ecall_M_Mode_Handler:
Ecall_U_Mode_Handler:
Break_Point_Handler:
SysTick_Handler:
SW_Handler:
TMR0_IRQHandler:
GPIOA_IRQHandler:
GPIOB_IRQHandler:
SPI0_IRQHandler:
BB_IRQHandler:
LLE_IRQHandler:
USB_IRQHandler:
TMR1_IRQHandler:
TMR2_IRQHandler:
UART0_IRQHandler:
UART1_IRQHandler:
RTC_IRQHandler:
ADC_IRQHandler:
I2C_IRQHandler:
PWMX_IRQHandler:
TMR3_IRQHandler:
UART2_IRQHandler:
UART3_IRQHandler:
WDOG_BAT_IRQHandler:
1:
	j 1b

	.section	.handle_reset,"ax",@progbits
	.weak	handle_reset
	.align	1
handle_reset:
	/* PHASE 0xC0 — after reset-status capture. No gp/sp yet, so use direct
	 * SRAM address (0x20005800). Manual LUI+SB sequence; no temporary
	 * gp-relative addressing.
	 *
	 * Capture R8_RESET_STATUS @ 0x40001044 BEFORE any other init so the
	 * reset cause (SW / RPOR / WTR / MR / LRM[0-2] / GPWSM per CH59x
	 * SYS_ResetStaTypeDef) is preserved across BSS clear. Stashed to
	 * 0x20005801; the fault handler writes 0x20005804.. on faults.
	 */
	li t1, 0x40001044
	lb t0, 0(t1)
	li t1, 0x20005801
	sb t0, 0(t1)

	li t0, 0xC0
	li t1, 0x20005800
	sb t0, 0(t1)

.option push
.option	norelax
	la gp, __global_pointer$
.option	pop
1:
	la sp, _eusrstack

	/* Own the two fatal vectors as soon as gp/sp are initialized, before the
	 * normal flash-to-SRAM .highcode copy. OpenBoot hands off to the app at
	 * 0x2000 in machine mode; relying on whatever mtvec state it leaves would
	 * leave most startup outside N23's controlled policy. Install the strong
	 * flash handlers explicitly and publish mtvec only after both words are
	 * visible. The later .highcode copy rewrites the same values and the normal
	 * CSR setup reinstalls the same mtvec base.
	 *
	 * The tiny reset-entry-through-sp interval remains bootloader-owned; N23
	 * does not claim corrupt-stack independence for hardware exception entry. */
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

	/* PHASE 0xC1 — gp/sp and the early fatal vectors are ready. */
	li t0, 0xC1
	li t1, 0x20005800
	sb t0, 0(t1)

/* Load highcode code  section from flash to RAM */
2:
    la a0, _highcode_lma
    la a1, _highcode_vma_start
    la a2, _highcode_vma_end
    bgeu a1, a2, 2f
1:
    lw t0, (a0)
    sw t0, (a1)
    addi a0, a0, 4
    addi a1, a1, 4
    bltu a1, a2, 1b

	/* PHASE 0xC2 — after .highcode copy. */
	li t0, 0xC2
	li t1, 0x20005800
	sb t0, 0(t1)

/* Load data section from flash to RAM */
2:
	la a0, _data_lma
	la a1, _data_vma
	la a2, _edata
	bgeu a1, a2, 2f
1:
	lw t0, (a0)
	sw t0, (a1)
	addi a0, a0, 4
	addi a1, a1, 4
	bltu a1, a2, 1b
2:
	/* PHASE 0xC3 — after .data copy. */
	li t0, 0xC3
	li t1, 0x20005800
	sb t0, 0(t1)

	/* clear bss section */
	la a0, _sbss
	la a1, _ebss
	bgeu a0, a1, 2f
1:
	sw zero, (a0)
	addi a0, a0, 4
	bltu a0, a1, 1b
2:
	/* PHASE 0xC4 — after .bss zero-clear. NOTE: bss-clear would zero
	 * any in-bss sentinel; ours at 0x20005800 is in the gap (above
	 * _ebss=0x20003718, below stack start 0x20006000) so it survives. */
	li t0, 0xC4
	li t1, 0x20005800
	sb t0, 0(t1)

	/* pipeline + dynamic prediction reset */
	li t0, 0x1f
	csrw 0xbc0, t0
	/* nested interrupts + hardware stacking enable */
	li t0, 0x3
	csrw 0x804, t0

    li t0, 0x88
    csrw mstatus, t0
	la t0, _vector_base

	/* Reinstall mtvec = relocated vector base + absolute-address mode. */
    ori t0, t0, 3
	csrw mtvec, t0

	la t0, main
	csrw mepc, t0

	/* PHASE 0xC5 — written immediately BEFORE the mret handoff to main. This
	 * is the LAST sentinel the dongle writes; main() advances no further
	 * marker (the 0xA0+ scheme was the keyboard project's — see the header
	 * note, N23). 0xC5 persisting therefore means "startup reached the main
	 * handoff"; a wedge in the mret itself would also leave 0xC5, so it is not
	 * proof main ran. Anything below 0xC5 is a wedge earlier in startup. */
	li t0, 0xC5
	li t1, 0x20005800
	sb t0, 0(t1)

	mret
