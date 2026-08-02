/* Bridge75 Dongle Firmware Linker Script
 * Based on CH592 SDK Link.ld
 *
 * Flash base 0x2000: OpenBoot owns [0x0000, 0x2000) and hands off to the
 * application at 0x2000. The app region is [0x2000, 0x70000) — the whole
 * remaining code flash. There is no in-app staging region any more; updates
 * are performed inside OpenBoot over OBP, and OpenBoot's boot record lives
 * in DataFlash at offset 0x7000 (outside code flash).
 */

ENTRY( _start )

__stack_size = 2048;
PROVIDE( _stack_size = __stack_size );

MEMORY
{
    /* RAM stops at 0x200067F0: OpenBoot reserves the top 16 bytes of the
     * 26 KB SRAM (0x200067F0..0x200067FF) for its boot-request word. */
    FLASH (rx)  : ORIGIN = 0x00002000, LENGTH = 0x6E000
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

    /* ODG2 remains a named output section for post-link finalization. */
    .dongle_id ORIGIN(FLASH) + 0x20 :
    {
        _dongle_id_start = .;
        KEEP(*(.dongle_id))
        _dongle_id_end = .;
    } >FLASH

    .highcodelalign :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_lma = .);
    } >FLASH AT>FLASH

    .highcode :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_vma_start = .);
        *(.vector);
        KEEP(*(SORT_NONE(.vector_handler)))
        *(.highcode);
        *(.highcode.*);
        . = ALIGN(4);
        PROVIDE(_highcode_vma_end = .);
    } >RAM AT>FLASH

    .text :
    {
        . = ALIGN(4);
        KEEP(*(SORT_NONE(.handle_reset)))
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata*)
        *(.sdata2.*)
        *(.glue_7)
        *(.glue_7t)
        *(.gnu.linkonce.t.*)
        . = ALIGN(4);
    } >FLASH AT>FLASH

    .fini :
    {
        KEEP(*(SORT_NONE(.fini)))
        . = ALIGN(4);
    } >FLASH AT>FLASH

    PROVIDE( _etext = . );
    PROVIDE( _eitcm = . );

    .preinit_array :
    {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } >FLASH AT>FLASH

    .init_array :
    {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
        PROVIDE_HIDDEN (__init_array_end = .);
    } >FLASH AT>FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } >FLASH AT>FLASH

    .ctors :
    {
        KEEP (*crtbegin.o(.ctors))
        KEEP (*crtbegin?.o(.ctors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
        KEEP (*(SORT(.ctors.*)))
        KEEP (*(.ctors))
    } >FLASH AT>FLASH

    .dtors :
    {
        KEEP (*crtbegin.o(.dtors))
        KEEP (*crtbegin?.o(.dtors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
        KEEP (*(SORT(.dtors.*)))
        KEEP (*(.dtors))
    } >FLASH AT>FLASH

    .dalign :
    {
        . = ORIGIN(RAM) + SIZEOF(.highcode);
    } >RAM AT>FLASH

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
        PROVIDE( __global_pointer$ = . + 0x800 );
        *(.sdata .sdata.*)
        *(.gnu.linkonce.s.*)
        . = ALIGN(8);
        *(.srodata.cst16)
        *(.srodata.cst8)
        *(.srodata.cst4)
        *(.srodata.cst2)
        *(.srodata .srodata.*)
        . = ALIGN(4);
        PROVIDE( _edata = .);
    } >RAM AT>FLASH

    .bss :
    {
        . = ALIGN(4);
        PROVIDE( _sbss = .);
        *(.sbss*)
        *(.gnu.linkonce.sb.*)
        *(.bss*)
        *(.gnu.linkonce.b.*)
        *(COMMON*)
        . = ALIGN(4);
        PROVIDE( _ebss = .);
    } >RAM AT>FLASH

    PROVIDE( _end = _ebss);
    PROVIDE( end = . );

    /* N23 retained-fault block. The four-byte prefix stays owned by
     * startup_CH592_phased.S; platform_ch592.c contributes the 44-byte
     * versioned record. NOLOAD prevents startup copy/zeroing, and KEEP plus
     * the assertions below make layout drift a link failure. */
    .fault_keep 0x20005800 (NOLOAD) :
    {
        _fault_keep_start = .;
        . = . + 4;
        _fault_record_start = .;
        KEEP(*(.noinit.ch592_fault))
        _fault_record_end = .;
        _fault_keep_end = .;
    } >RAM

    /* CODEREVIEW N12b: cap the newlib heap ceiling at the fault-retention
     * block instead of the stack region base, so a large-enough _sbrk growth
     * (vendor stdio retains puts -> __smakebuf_r -> _malloc_r) can no longer
     * grow the break THROUGH the 0x20005800 fault block. Only the SDK _sbrk
     * consumes _heap_end, so this changes just that bound's relocation — the
     * byte-changing half of N12 that was deferred out of the byte-identical
     * change 4. The N23 NOLOAD section above now defines the linker-owned
     * lower bound directly. */
    _heap_end = _fault_keep_start;

    .stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size :
    {
        . = ALIGN(4);
        PROVIDE(_susrstack = . );
        . = . + __stack_size;
        PROVIDE( _eusrstack = .);
    } >RAM

    /* Fail the link if .data/.bss grow into the reserved 2 KB stack. The .stack
     * section is placed at a FIXED top-of-RAM address, so the linker does NOT
     * auto-detect bss overrunning it -- this guards the tight CH592 stack floor
     * (BLE pair-stack margin) at build time. */
    ASSERT(_ebss <= _susrstack,
        "ERROR: .data/.bss overruns the reserved stack -- reduce RAM use")

    /* CODEREVIEW N12/N23: two fixed-address RAM regions are linker-owned so
     * nothing can silently grow into them.
     *
     * (1) Fault retention: fault_handler.S + the startup sentinel keep
     *     0x20005800..0x2000582F (startup prefix + versioned N23 record),
     *     which sits between _ebss and the stack region. The real NOLOAD
     *     section above plus the ASSERT below fail the link if .data/.bss
     *     ever reach it. N12b (done in change 10, the next CH592-codegen
     *     change): _heap_end is now capped at 0x20005800 (above), so the SDK
     *     _sbrk can no longer grow the runtime heap THROUGH this block either
     *     -- closing the residual that was deferred out of the byte-identical
     *     change 4 (vendor stdio retains puts -> __smakebuf_r -> _malloc_r).
     *
     * (2) CH59x BLE-lib RF arena: the library pokes fixed addresses
     *     0x200060A0..0x200060D0 (rf_task.c RF_STOCK_RF_ARENA_BASE[0..11])
     *     INSIDE the nominal 2 KB top-of-RAM stack region, so those bytes
     *     are NOT usable stack. _susrstack -- the guarded stack floor that
     *     begins immediately above the arena: the TRUE stack
     *     is 0x200060D0..0x200067F0 = 1824 B (RAM now tops out at the
     *     OpenBoot bootreq word). sp still initializes at _eusrstack (top
     *     of app RAM). The .stack section above still spans from 0x20005FF0
     *     for layout compatibility; these SYMBOLS carry the truth (the
     *     plain assignment overrides the section's PROVIDE). */
    _rf_arena_start   = 0x200060A0;
    _rf_arena_end     = 0x200060D0;
    _susrstack        = _rf_arena_end;

    ASSERT(_ebss <= _fault_keep_start,
        "ERROR: .data/.bss overruns the fault-retention block at 0x20005800")
    ASSERT(_fault_keep_start < _fault_keep_end && _rf_arena_start < _rf_arena_end,
        "ERROR: reserved-region bounds out of order")
    ASSERT(_susrstack == _rf_arena_end && _rf_arena_end <= _eusrstack,
        "ERROR: stack floor must sit exactly on top of the BLE RF arena")
    ASSERT(_eusrstack == 0x200067F0,
        "stack top must sit below the OpenBoot bootreq word")
    ASSERT(_heap_end <= _rf_arena_start,
        "ERROR: heap ceiling must stay below the BLE RF arena")
    ASSERT(_heap_end == _fault_keep_start,
        "ERROR: N12b heap cap must equal the fault-block start (0x20005800)")
    /* Pin the absolute address too: the fault/startup assembly hard-codes
     * 0x20005800, so a coordinated move of both linker symbols must still trip
     * this if the assembly is not moved with them. */
    ASSERT(_fault_keep_start == 0x20005800,
        "ERROR: fault block must stay at 0x20005800 (matches fault_handler.S / startup)")
    ASSERT(_fault_record_start == 0x20005804,
        "ERROR: N23 fault record must begin after the four-byte startup prefix")
    ASSERT(_fault_record_end == 0x20005830 && _fault_keep_end == 0x20005830,
        "ERROR: N23 fault record/layout size changed (matches fault_record.h)")

    /* CODEREVIEW F01 (+ Codex round-4): the EXACT loadable image size in bytes
     * (.data is the last loadable flash section; its LMA end is 4-aligned, which
     * is the true .bin length). flash_dongle.py pads the programmed image only to
     * the 4-byte flash-write unit (NOT 16) and commits that exact length, and the
     * device's IAP-write path already accepts 4-aligned chunk lengths -- so the
     * committed length equals this marker for a whole image, but a >=4-byte tail
     * truncation commits a SHORTER length that no longer matches this baked value
     * and the commit rejects it (0xEB). (An earlier 16-byte round-up aliased 1-15
     * trailing bytes back to the full length; the host also exact-checks the raw
     * file length against this marker to catch a sub-4-byte truncation.)
     * dongle_id.c stamps this into the image-id header's image_len field. */
    _dongle_image_len = ALIGN(LOADADDR(.data) + SIZEOF(.data), 4) - ORIGIN(FLASH);

    /* CODEREVIEW F01: pin the image-id header so the firmware can trust its
     * fixed staged offset, and keep the startup .highcode copy correct. */
    ASSERT(ORIGIN(FLASH) == 0x2000,
        "F01: app base must be 0x2000 (OpenBoot handoff) to match DONGLE_IAP_APP_BASE")
    ASSERT(_dongle_id_start == ORIGIN(FLASH) + 0x20,
        "F01: image-id header must be pinned at image offset 0x20")
    ASSERT(_dongle_id_end - _dongle_id_start == 0x20,
        "ODG2 image-id header must be exactly 32 bytes")
    ASSERT(_einit <= _dongle_id_start,
        "F01: .init overruns the image-id header offset")
    ASSERT(_highcode_lma == LOADADDR(.highcode),
        "F01: highcode LMA must follow the image-id header (startup copies via _highcode_lma)")
}
