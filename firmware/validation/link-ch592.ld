/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Link script for the CH592 AES validation image.
 *
 * STANDALONE, AT 0x0 -- see link-ch570.ld for the full rationale and the
 * warning that flashing this ERASES OPENBOOT.
 *
 * Simpler than the CH570 script in one respect: this arm uses the vendor's
 * stock startup rather than the production reset handler, so there is no
 * retained startup-marker block to reserve. CH592's V4C core has no CORECFGR
 * to preserve, which is the only reason the production reset owner was
 * load-bearing on the other chip.
 */
ENTRY( _start )

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 0x70000
    /* Matches production's RAM window. CH592 can be configured with a larger
     * SRAM split, but production does not, and a validation image is the wrong
     * place to discover that a different split behaves differently. */
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 0x67F0
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

    /* Retained across a warm reset, exactly as in link-ch570.ld. This section
     * MUST be declared: aes_validate.c puts validate_keep[] and
     * validate_saved[] in `.validate_keep`, and without an output section for
     * it the linker emits an ORPHAN. That is not a cosmetic difference -- the
     * orphan came out as a LOADED data section carrying 684 B of flash image,
     * and whether it survived a reset depended on where the linker happened to
     * place it relative to the startup copy. Retention has to be a property of
     * the link, not a coincidence of it. */
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

    ASSERT((ORIGIN(RAM) + LENGTH(RAM) - _validate_keep_end) >= 0x800,
           "CH592 validation image leaves less than a 2 KiB stack")
}
