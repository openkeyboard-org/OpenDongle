/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Heap watermark instrumentation, measurement builds only
 * (EXTRA_CFLAGS=-DDONGLE_HEAP_WATERMARK=1).
 *
 * Purpose: put a number on how much of the BLE library's heap (MEM_BUF,
 * BLE_MEMHEAP_SIZE bytes) this firmware ever touches, so the size can be
 * chosen from a measurement instead of the SDK default.
 *
 * Method: paint the whole heap with a pattern before the library is handed
 * it, run the exercise matrix (a fresh pairing with a key exchange, typing,
 * a reconnect), then count what still holds the pattern. The heap is not a
 * stack, so three figures are reported: the untouched total, the highest
 * touched byte, and the largest untouched run with where it starts. Only an
 * untouched run that reaches the end of the heap can be given back by
 * shrinking it.
 *
 * A word the firmware wrote the pattern value into reads as untouched; the
 * pattern is chosen to be an unlikely pointer, length or payload.
 *
 * Readout: IAP command 0x9B (iap.c), printed by `opendongle --crypt-diag`.
 */

#ifndef HEAP_WATERMARK_H
#define HEAP_WATERMARK_H

#include <stdint.h>

#ifndef DONGLE_HEAP_WATERMARK
#define DONGLE_HEAP_WATERMARK 0
#endif

#if DONGLE_HEAP_WATERMARK

#define HEAP_WATERMARK_PATTERN 0x5AC3E17Bu
#define HEAP_WATERMARK_BYTES   28u

/* Paint the heap. Call before the library is initialised. */
void heap_watermark_paint(void);

/* Record the figures as they stand once initialisation is complete, so the
 * readout can tell what start-up took from what running took. */
void heap_watermark_mark_boot(void);

/* Fill `out` (HEAP_WATERMARK_BYTES), little-endian words:
 *   [0]  heap size, bytes
 *   [4]  untouched, bytes
 *   [8]  end of the highest touched word, as an offset from the heap start
 *   [12] largest untouched run, bytes
 *   [16] where that run starts, offset
 *   [20] untouched at the end of initialisation, bytes
 *   [24] end of the highest touched word at the end of initialisation
 * Returns the number of bytes written. */
uint8_t heap_watermark_fill(uint8_t *out);

#endif /* DONGLE_HEAP_WATERMARK */

#endif /* HEAP_WATERMARK_H */
