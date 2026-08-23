/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 *
 * Validation-image stand-in for the per-chip dongle_target.h.
 *
 * rf_crypt.c includes "dongle_target.h" so a product target can flip the
 * bench diagnostics on (see rf_crypt.h for the defaults it overrides). The
 * validation arms must compile rf_crypt in its SHIPPING shape -- proving the
 * bench-diagnostic variant would not validate the cipher path products run --
 * so this header deliberately defines none of those switches and exists only
 * to satisfy the include.
 *
 * It shadows the real per-chip headers by include-path order: every arm lists
 * `-I .` first (validation/Makefile), ahead of `-I ../ch570/src` on the CH570
 * arms. That shadowing is also what keeps the CH570 arms buildable at all --
 * ch570/src/dongle_target.h static-asserts against CH570_SYSCLK_HZ, a -D that
 * only the production Makefile supplies.
 */

#ifndef DONGLE_TARGET_VALIDATION_H
#define DONGLE_TARGET_VALIDATION_H

/* Intentionally empty: rf_crypt.h's defaults (all bench switches off) are the
 * shipping configuration. */

#endif /* DONGLE_TARGET_VALIDATION_H */
