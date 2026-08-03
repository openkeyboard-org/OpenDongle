/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * CODEREVIEW F01 — the app-image identity header instance.
 *
 * Placed by the linker at ORIGIN(FLASH) + DONGLE_IMAGE_ID_OFF (0x1020) in the
 * .dongle_id section, which link.ld pins and asserts. `used` keeps it despite
 * -fdata-sections/--gc-sections and it having no C references on the running
 * image (the IAP commit reads the STAGED copy out of flash, not this symbol).
 *
 * image_kind is APP only when the build actually carries USB+IAP
 * (DONGLE_IMAGE_HAS_IAP, set by the product Makefile recipes); an RF-only build
 * gets NON_APP so the IAP commit refuses to promote it. image_len is the
 * loadable image size, provided by the linker symbol _dongle_image_len (==
 * LOADADDR(.data)+SIZEOF(.data)-ORIGIN(FLASH), i.e. the exact byte length the
 * host flashes), so the commit can reject a truncated image.
 */
#include "dongle_platform.h"    /* DONGLE_CHIP_FAMILY_ID, DONGLE_IAP_APP_BASE */
#include "dongle_image_id.h"

#if DONGLE_IAP_IMAGE_ID

#ifndef DONGLE_IMAGE_HAS_IAP
#define DONGLE_IMAGE_HAS_IAP 0
#endif

/* Linker-provided; only its ADDRESS carries the value (the loadable image size). */
extern const char _dongle_image_len[];

const dongle_image_id_t
__attribute__((section(".dongle_id"), used, aligned(4)))
dongle_image_id = {
    { DONGLE_IMAGE_ID_MAGIC0, DONGLE_IMAGE_ID_MAGIC1,
      DONGLE_IMAGE_ID_MAGIC2, DONGLE_IMAGE_ID_MAGIC3 },
    DONGLE_IMAGE_ID_FORMAT,
    DONGLE_CHIP_FAMILY_ID,
    (DONGLE_IMAGE_HAS_IAP ? DONGLE_IMAGE_KIND_APP : DONGLE_IMAGE_KIND_NON_APP),
    DONGLE_IMAGE_ID_LEN,
    DONGLE_IAP_APP_BASE,
    (uint32_t)(uintptr_t)_dongle_image_len,
    0u, /* finalized after link */
    DONGLE_BUILD_ID,
    0u,
    0u,
};

#endif /* DONGLE_IAP_IMAGE_ID */
