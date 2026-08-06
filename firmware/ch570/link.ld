ENTRY( _start )

/* The MEMORY block is GENERATED per slot into $(BUILD)/memory.ld and pulled in
 * here, the same way OpenBoot parameterises its own link script. It carries no
 * literal addresses, so this file is identical for slot A and slot B and the
 * two builds differ only in what the Makefile derived from OpenBoot.
 *
 * Layout, for orientation (CH570D, 240 KB program flash, OB_APP_END clamped to
 * 0x3A000 so OBP cannot reach the bond page):
 *
 *   slot A  [0x02000, 0x1D000)   record [0x1D000, 0x1E000)
 *   slot B  [0x1E000, 0x39000)   record [0x39000, 0x3A000)
 *   bond    0x3A000              spare  0x3B000
 *
 * LENGTH is the slot CAPACITY, not the slot size: linking across the record
 * block still boots (the bootloader CRCs only img_len bytes) and is then
 * destroyed by the first COMMIT, which erases that block to store the record.
 *
 * RAM stops at 0x20002FF0: OpenBoot reserves the top 16 bytes of the 12 KB
 * SRAM (0x20002FF0..0x20002FFF) for its boot-request word. */
INCLUDE memory.ld

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

    /* ODG2 is a distinct output section so the post-link finalizer can patch
     * its image length/CRC/build identity without rebuilding the application. */
    .dongle_id ORIGIN(FLASH) + 0x20 :
    {
        _dongle_id_start = .;
        KEEP(*(.dongle_id))
        _dongle_id_end = .;
    } >FLASH

    .highcode_initlalign :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_init_lma = .);
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
        *(.vector);
        *(.highcode);
        *(.highcode.*);
        . = ALIGN(4);
        PROVIDE(_highcode_vma_end = .);
    } >RAM AT>FLASH

    .text :
    {
        . = ALIGN(4);
        KEEP(*(.handle_reset_n23))
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata*)
        *(.sdata2.*)
        *(.glue_7)
        *(.glue_7t)
        *(.gnu.linkonce.t.*)
        /* Keep the vendor's orphan vector/fini tails inside this loadable
         * output section, then enforce the 4 KiB minimum app envelope that
         * the ODG2 image_len and manifest contracts advertise.  Extending a
         * PROGBITS section is important: objcopy's --pad-to does not extend a
         * CH570 ELF after seeing the later SRAM-VMA .highcode segment. */
        KEEP(*(.vector_handler))
        KEEP(*(SORT_NONE(.fini)))
        . = ALIGN(4);
        . = MAX(ABSOLUTE(.), ORIGIN(FLASH) + 0x1000);
    } >FLASH AT>FLASH

    PROVIDE(_etext = .);
    PROVIDE(_eitcm = .);

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } >FLASH AT>FLASH

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } >FLASH AT>FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } >FLASH AT>FLASH

    .ctors :
    {
        KEEP(*crtbegin.o(.ctors))
        KEEP(*crtbegin?.o(.ctors))
        KEEP(*(EXCLUDE_FILE (*crtend.o *crtend?.o) .ctors))
        KEEP(*(SORT(.ctors.*)))
        KEEP(*(.ctors))
    } >FLASH AT>FLASH

    .dtors :
    {
        KEEP(*crtbegin.o(.dtors))
        KEEP(*crtbegin?.o(.dtors))
        KEEP(*(EXCLUDE_FILE (*crtend.o *crtend?.o) .dtors))
        KEEP(*(SORT(.dtors.*)))
        KEEP(*(.dtors))
    } >FLASH AT>FLASH

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
    } >RAM AT>FLASH

    /* N23 retained-fault block. The reset owner writes the four-byte startup
     * prefix; fault_record.c contributes the 44-byte transactional record.
     * Keep it dynamic immediately above .bss so the production image does not
     * lose RAM to a fixed-address hole. NOLOAD excludes it from copy/clear. */
    .fault_keep (NOLOAD) :
    {
        . = ALIGN(4);
        _fault_keep_start = .;
        . = . + 4;
        _fault_record_start = .;
        KEEP(*(.noinit.ch570_fault))
        _fault_record_end = .;
        _fault_keep_end = .;
    } >RAM

    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        PROVIDE(_snoinit = .);
        KEEP(*(.noinit))
        KEEP(*(.noinit.*))
        . = ALIGN(4);
        PROVIDE(_enoinit = .);
    } >RAM

    PROVIDE(_end = _enoinit);
    PROVIDE(end = .);
    /* Keep the production stack floor above the 1,600-byte cold-boot entropy
     * window (_end+64 through _end+1599). */
    PROVIDE(CH570_STACK_FLOOR = 0x800);
    ASSERT(CH570_STACK_FLOOR >= 0x640,
           "CH570 stack floor is smaller than boot-entropy read window")
    ASSERT((ORIGIN(RAM) + LENGTH(RAM) - _end) >= CH570_STACK_FLOOR,
           "CH570 SRAM leaves less than the selected stack floor")
    ASSERT(_fault_record_start == _fault_keep_start + 4,
           "N23: CH570 fault record must follow the startup prefix")
    ASSERT(ch570_fault_record == _fault_record_start,
           "N23: CH570 fault record symbol/layout drifted")
    ASSERT(_fault_record_end - _fault_record_start == 44,
           "N23: CH570 fault record must remain 44 bytes")
    ASSERT(_fault_keep_end - _fault_keep_start == 48,
           "N23: CH570 retained block must remain 48 bytes")

    .stack ORIGIN(RAM)+LENGTH(RAM) :
    {
        . = ALIGN(4);
        PROVIDE(_eusrstack = .);
    } >RAM

    ASSERT(_eusrstack == 0x20002FF0,
        "stack top must sit below the OpenBoot bootreq word")

    /* CODEREVIEW F01 (+ Codex round-4): the EXACT loadable image size in bytes
     * (.data is the last loadable flash section and its LMA end is 4-aligned).
     * flash_dongle.py pads the programmed image only to the 4-byte
     * flash-write unit and commits that exact length, and the device IAP-write
     * path already accepts 4-aligned chunk lengths -- so a whole image matches
     * this baked marker, but a >=4-byte tail truncation commits a shorter length
     * that no longer matches and the commit rejects it (0xEB). The host also
     * exact-checks the raw file length against this marker (catches a sub-4-byte
     * truncation). dongle_id.c stamps this into the header's image_len. */
    /* Keep every raw application envelope at least 4 KiB for host validation. */
    _dongle_image_len = MAX(
        ALIGN(LOADADDR(.data) + SIZEOF(.data), 4) - ORIGIN(FLASH), 0x1000);

    /* CODEREVIEW F01: pin the image-id header at a fixed staged offset and keep
     * the startup .highcode copy correct.
     *
     * Under A/B this can no longer be "== 0x2000": slot B links at 0x1E000 and
     * is equally valid. What must still hold is that the app links at one of
     * OpenBoot's TWO slot bases and nowhere else - a garbled geometry read that
     * produced some other address would otherwise link cleanly and then be
     * unbootable, because the bootloader only ever jumps to a slot base.
     * Both bases are emitted into the generated memory.ld. */
    ASSERT(ORIGIN(FLASH) == DONGLE_SLOT_A_BASE || ORIGIN(FLASH) == DONGLE_SLOT_B_BASE,
        "F01: app must link at one of OpenBoot's slot bases (see memory.ld)")
    ASSERT(_dongle_id_start == ORIGIN(FLASH) + 0x20,
        "F01: image-id header must be pinned at image offset 0x20")
    ASSERT(_dongle_id_end - _dongle_id_start == 0x20,
        "ODG2 image-id header must be exactly 32 bytes")
    ASSERT(_einit <= _dongle_id_start,
        "F01: .init overruns the image-id header offset")
    ASSERT(_highcode_lma == LOADADDR(.highcode),
        "F01: highcode LMA must follow the image-id header (startup copies via _highcode_lma)")
}
