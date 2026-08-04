/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Link script for the CH570/CH572 AES validation image.
 *
 * STANDALONE, AT 0x0. This is NOT the production layout and is not composed
 * with OpenBoot: the image owns the whole part and runs directly from reset.
 *
 *   !! FLASHING THIS ERASES OPENBOOT. A device that has run the validation
 *   !! image must be re-flashed with a factory image before it is a dongle
 *   !! again. run_aes_validation.py says so before it writes anything.
 *
 * Linking at 0x2000 under OpenBoot was the alternative and was rejected: it
 * would drag in the ODG2 image-id header, the image-length/CRC finalizer and
 * the boot-record contract, none of which have anything to do with the cipher,
 * and each of which is a way for the run to fail for reasons that are not the
 * cipher's fault.
 *
 * The production script's assertions are deliberately absent here. They pin the
 * retained fault block, the RF arena and the stack floor -- all properties of
 * the shipping firmware's memory pressure, which this image does not reproduce
 * and must not pretend to. SRAM headroom for a backend is proved by `make all`
 * linking the real firmware, not by this image linking.
 *
 * What IS reproduced faithfully, because it changes the measurement:
 *   - .highcode lives in RAM, so an SRAM-resident cipher kernel is placed as it
 *     would be in production and .text stays flash-resident.
 *   - the vendor vector table and this firmware's own reset handler, so
 *     CORECFGR is written exactly as production writes it.
 */
ENTRY( _start )

MEMORY
{
    /* CH570D/CH572 program flash. The app region ends where the production
     * bond page begins, so a validation image can never scribble on bonds
     * even though it owns everything below. */
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 0x3A000
    /* Matches the production RAM window exactly. The top 16 bytes belong to
     * OpenBoot's boot-request word in production; keeping the same bound here
     * costs nothing and removes a difference nobody would think to check. */
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 0x2FF0
}

SECTIONS
{
    .init :
    {
        _sinit = .;
        . = ALIGN(4);
        KEEP(*(SORT_NONE(.init)))
        . = ALIGN(4);
        _einit = .;
    } >FLASH AT>FLASH

    .highcodelalign :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_lma = .);
    } >FLASH AT>FLASH

    /* Copied to RAM by handle_reset. The 1024 alignment is the vector table's
     * requirement, not a preference. */
    .highcode :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_vma_start = .);
        . = ALIGN(1024);
        KEEP(*(.vector));
        *(.highcode);
        *(.highcode.*);
        . = ALIGN(4);
        PROVIDE(_highcode_vma_end = .);
    } >RAM AT>FLASH

    .text :
    {
        . = ALIGN(4);
        /* The strong reset owner. Without KEEP, --gc-sections drops it and the
         * vendor's weak handle_reset runs instead -- which would boot, and
         * would silently NOT write CORECFGR. */
        KEEP(*(.handle_reset_n23))
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata*)
        *(.sdata2.*)
        *(.gnu.linkonce.t.*)
        KEEP(*(.vector_handler))
        KEEP(*(SORT_NONE(.fini)))
        . = ALIGN(4);
    } >FLASH AT>FLASH

    PROVIDE(_etext = .);

    .dlalign :
    {
        . = ALIGN(4);
        PROVIDE(_data_lma = .);
    } >FLASH AT>FLASH

    .data :
    {
        . = ALIGN(4);
        PROVIDE(_data_vma = .);
        *(.gnu.linkonce.r.*)
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
        *(.gnu.linkonce.s.*)
        . = ALIGN(8);
        *(.srodata.cst16)
        *(.srodata.cst8)
        *(.srodata.cst4)
        *(.srodata.cst2)
        *(.srodata .srodata.*)
        . = ALIGN(4);
        PROVIDE(_edata = .);
    } >RAM AT>FLASH

    .bss :
    {
        . = ALIGN(4);
        PROVIDE(_sbss = .);
        *(.sbss*)
        *(.gnu.linkonce.sb.*)
        *(.bss*)
        *(.gnu.linkonce.b.*)
        *(COMMON*)
        . = ALIGN(4);
        PROVIDE(_ebss = .);
    } >RAM

    /* reset_handler_ch570.S writes its startup phase markers to the first two
     * bytes here (STARTUP_MARKER / the reset-cause capture). Production backs
     * this with a 48-byte retained block owned by fault_record.c; the
     * validation image links no fault recorder, so it only has to reserve the
     * four bytes the reset handler actually touches. NOLOAD keeps it out of
     * the copy/clear loops, which is what makes it retained at all. */
    .fault_keep (NOLOAD) :
    {
        . = ALIGN(4);
        _fault_keep_start = .;
        . = . + 4;
        _fault_keep_end = .;
    } >RAM

    /* Survives a warm reset: NOLOAD keeps it out of the startup copy, and it
     * sits above _ebss so the reset handler's BSS clear cannot reach it. This
     * is what lets the image count its own boots and so tell a reset loop from
     * a hang -- a distinction the log alone cannot make, because a reset
     * restarts the log from the beginning and looks identical to a stall. */
    .validate_keep (NOLOAD) :
    {
        . = ALIGN(4);
        _validate_keep_start = .;
        KEEP(*(.validate_keep))
        . = ALIGN(4);
        _validate_keep_end = .;
    } >RAM

    PROVIDE(_end = _validate_keep_end);
    PROVIDE(end = _validate_keep_end);

    .stack ORIGIN(RAM) + LENGTH(RAM) :
    {
        . = ALIGN(4);
        PROVIDE(_eusrstack = .);
    } >RAM

    /* The one memory assertion worth keeping: the image plus a usable stack
     * has to fit. 2 KiB matches the production stack floor, so a validation
     * image that fits here would also have stack headroom in production. */
    ASSERT((ORIGIN(RAM) + LENGTH(RAM) - _validate_keep_end) >= 0x800,
           "CH570 validation image leaves less than a 2 KiB stack")
}
