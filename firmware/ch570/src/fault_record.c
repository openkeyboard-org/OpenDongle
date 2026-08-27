/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fault_record.h"

/* Defined separately so the linker-owned retained layout stays explicit. */
volatile ch570_fault_record_t ch570_fault_record
    __attribute__((section(".noinit.ch570_fault"), used, aligned(4)));
