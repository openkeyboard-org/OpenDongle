/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wire format of the AES validation log: the ONE definition, shared by the
 * on-device harness (aes_validate.c) and the host reader (read_aes_log.py).
 *
 * The harness cannot print. ch32fun's printf blocks at boot when no debug
 * terminal is attached, which silently stalls the probe -- so results are
 * written to a word array in RAM and read back over the debug link after the
 * run. Everything here is a plain scalar `#define` for exactly one reason:
 * read_aes_log.py PARSES THIS FILE for the marker values rather than declaring
 * its own copies. Introduce an expression, an enum, or a function-like macro
 * for any of the scalars below and the reader stops being able to read them.
 * Index-derived tags (vectors, timings) are therefore expressed as a BASE the
 * reader can mask against, not as `TAG(n)` macros.
 *
 * WHY MARKERS AND NOT FIXED OFFSETS. A record is walked marker by marker, so
 * the reader survives the harness growing new sections; a reader keyed to word
 * offsets would silently misread every field after the first insertion. The
 * one rule that makes this work: every record is FIXED WIDTH for its marker.
 * The bench original violated this -- its fourth vector emitted a tag and a
 * flag while the first three emitted a tag, four ciphertext words and a flag --
 * so a walking reader desynchronised at exactly the point the log got
 * interesting. Vectors here are uniformly AES_LOG_VECTOR_WORDS wide.
 *
 * WHERE THE LOG LIVES. `aes_log[]` is an ordinary exported array in .bss and
 * the runner resolves its address from the ELF symbol table. It deliberately
 * does NOT live at a hard-coded address: the bench harness used a fixed
 * 0x20001000, which works only as long as nothing else in the image grows into
 * it, and that is not a property anyone checks or would notice breaking. A
 * symbol cannot collide with the linker's own allocation.
 */
#ifndef AES_LOG_FORMAT_H
#define AES_LOG_FORMAT_H

/* "SAE1". First word of every record; nothing is trusted without it. */
#define AES_LOG_MAGIC            0x53414531

/* Bumped whenever the meaning of an existing field changes. The reader refuses
 * a version it does not know rather than guessing -- a misparsed pass is worse
 * than a refusal. */
#define AES_LOG_VERSION          2

/* Capacity in words. The harness stops writing at this bound and sets the
 * overflow flag rather than running off the end of the array; a truncated log
 * that says so beats a corrupted neighbour that does not. */
#define AES_LOG_WORDS            160

/*
 * The last four words are reserved for a fatal-fault record and are never used
 * by the body: magic, mcause, mepc, mtval.
 *
 * The fault vectors originally just spun, on the reasoning that a fault would
 * show up as a log with no end marker and that was enough. It was not. When
 * both CH570 arms stopped partway through the differential, "incomplete" could
 * not distinguish a fault from a genuine hang, and the 32-block checkpoint
 * spacing localised it no better than a 32-block window. mepc names the
 * instruction outright.
 */
#define AES_LOG_FAULT_WORDS      4
#define AES_LOG_FAULT_SLOT       (AES_LOG_WORDS - AES_LOG_FAULT_WORDS)
/* Low byte distinguishes the vector: 1 = NMI, 2 = HardFault. */
#define AES_LOG_FAULT_MAGIC      0xFA017E00
#define AES_LOG_FAULT_MASK       0xFFFFFF00
#define AES_LOG_FAULT_NMI        1
#define AES_LOG_FAULT_HARD       2

/* Header, in order: magic, version, start sentinel, core clock Hz, backend id,
 * build id. Fixed because it precedes the marker walk. */
#define AES_LOG_HEADER_WORDS     6

/* Start-of-run sentinel, third header word. Distinguishes "the harness ran"
 * from "this RAM happens to begin with something magic-looking". */
#define AES_LOG_START            0xC0DE5A45

/* Which backend produced this record. The whole point of the suite is that all
 * of them yield identical ciphertext, so every record must say which one it
 * was -- otherwise a cross-arm comparison is comparing unlabelled numbers. */
#define AES_LOG_BACKEND_UNKNOWN  0
#define AES_LOG_BACKEND_ASM_A    1
#define AES_LOG_BACKEND_ASM_F    2
#define AES_LOG_BACKEND_C        3
#define AES_LOG_BACKEND_HW       4

/*
 * Vector records: AES_LOG_VECTOR_BASE | n, then 4 ciphertext words (little
 * endian), then a result word. Six words, always.
 */
#define AES_LOG_VECTOR_BASE      0x56000000
#define AES_LOG_VECTOR_MASK      0xFFFFFF00
#define AES_LOG_VECTOR_WORDS     6

/* Result-word bits. Kept separate so the reader can distinguish "the cipher is
 * wrong" from "the engine never ran", which a single boolean cannot. */
#define AES_LOG_RESULT_BYTES_OK  0x1  /* ciphertext matched the expectation */
#define AES_LOG_RESULT_STATUS_OK 0x2  /* the seam returned HAL_AES_OK */

/* Vector ids. Stable: a report from an old firmware must still be readable, so
 * these are only ever appended to, never renumbered. 1-6 are published
 * known-answer vectors; 7-11 are contract properties from hal_aes.h. */
#define AES_LOG_VEC_FIPS197_C1   1
#define AES_LOG_VEC_ALL_ZERO     2
#define AES_LOG_VEC_SP80038A_1   3
#define AES_LOG_VEC_SP80038A_2   4
#define AES_LOG_VEC_SP80038A_3   5
#define AES_LOG_VEC_SP80038A_4   6
#define AES_LOG_VEC_IN_PLACE     7  /* out == in */
#define AES_LOG_VEC_OVERLAP      8  /* out == in + 1 */
#define AES_LOG_VEC_SET_KEY_TWICE 9 /* set_key is idempotent */
#define AES_LOG_VEC_KEY_CHANGE   10 /* K1 -> K2 -> K1 leaves no residue */
#define AES_LOG_VEC_TWICE        11 /* no cross-block chaining state */
#define AES_LOG_VEC_COUNT        11

/* End of the vector section, followed by one word: the AND of every result. */
#define AES_LOG_KAT_END          0x5645FFFF

/*
 * The 512-block differential. ENTRY and EXIT are a matched pair, and that is
 * the point of having both: a bare checksum cannot distinguish "the
 * differential was skipped" from "it hung partway through", and those call for
 * completely different debugging. Between them the harness emits one running
 * checkpoint every AES_LOG_DIFF_CHECKPOINT blocks, so a divergence localises to
 * a window instead of only showing up as a wrong final number.
 *
 * After EXIT: block count, final fold, then 4 words of the last ciphertext.
 */
#define AES_LOG_DIFF_ENTER       0xD1FF1111
#define AES_LOG_DIFF_EXIT        0xD1FF0000
#define AES_LOG_DIFF_COUNT       512
#define AES_LOG_DIFF_CHECKPOINT  32
#define AES_LOG_DIFF_SEED        0x12345678
#define AES_LOG_DIFF_FNV_BASIS   0x811C9DC5
#define AES_LOG_DIFF_FNV_PRIME   0x01000193

/* Timing records: AES_LOG_TIMING_BASE | n, then one word of core cycles.
 * Recorded as regression signals, never as pass/fail -- a slow cipher is a
 * finding to investigate, not a broken one. */
#define AES_LOG_TIMING_BASE      0x71000000
#define AES_LOG_TIMING_MASK      0xFFFFFF00
#define AES_LOG_TIMING_WORDS     2
#define AES_LOG_TIME_OVERHEAD    0  /* the measurement's own cost */
#define AES_LOG_TIME_KEY_EXPAND  1  /* hal_aes_set_key, averaged */
#define AES_LOG_TIME_BLOCK       2  /* hal_aes_encrypt_block, averaged */

/* CORECFGR, followed by the value STARTUP WRITES -- not a value read back.
 * Bit 3 changes the cipher's cost by ~15x on the flash-resident backends, so a
 * cycle count means nothing without it. This was originally a csrr 0xbc0, which
 * RESETS a CH570: production only ever writes that CSR. See the harness. */
#define AES_LOG_CORECFGR         0xCACE0000

/* End of run. Its absence means the harness died partway; the reader reports
 * that as an incomplete record rather than scoring what it managed to read. */
#define AES_LOG_END              0xD09EFFFF

/*
 * Retained-memory magics. `aes_log` lives in .bss and is cleared by every
 * reset, so a part that reboots mid-run presents an identical truncated log
 * each time and looks exactly like a hang. Two retained blocks fix that:
 *
 *   validate_keep  - boot count, reset cause, watchdog state, furthest stage.
 *                    Distinguishes "reset loop" from "hang", which the log
 *                    alone provably cannot.
 *   validate_saved - a snapshot of the last COMPLETED log. The magic is
 *                    written last, so an interrupted copy is never mistaken
 *                    for a whole one.
 */
#define AES_LOG_KEEP_MAGIC       0x564B3031  /* "VK01" */
#define AES_LOG_SAVED_MAGIC      0x56533031  /* "VS01" */

/* Set in place of AES_LOG_END if the harness ran out of log space. */
#define AES_LOG_OVERFLOW         0x0F10FF00

#endif /* AES_LOG_FORMAT_H */
