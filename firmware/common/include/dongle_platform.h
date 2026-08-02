#ifndef DONGLE_PLATFORM_H
#define DONGLE_PLATFORM_H

#include <stdint.h>

/*
 * Target ports provide dongle_target.h in their local include path. It carries
 * compile-time identity and flash-map constants; this common header declares
 * the runtime hooks implemented by each port.
 */
#include "dongle_target.h"
#include "dongle_status.h"

#ifndef DONGLE_FW_VERSION
#error "DONGLE_FW_VERSION must be defined by dongle_target.h"
#endif

#ifndef DONGLE_CHIP_FAMILY_ID
#error "DONGLE_CHIP_FAMILY_ID must be defined by dongle_target.h"
#endif

/* When 1, the device advertises + services USB remote wakeup (bmAttributes
 * bit 5, SET/CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP), GET_STATUS bit 1, and the
 * resume-signalling path). Target ports should only set this when their main
 * loop calls USB_ServiceRemoteWake() and their USB HAL can drive resume K. */
#ifndef DONGLE_USB_REMOTE_WAKEUP
#define DONGLE_USB_REMOTE_WAKEUP 0
#endif

/* When 1, the shared RF task (common/src/rf_task.c) is linked into this build,
 * so its public API (e.g. RF_TombstoneBond) is callable. */
#ifndef DONGLE_HAS_RF
#define DONGLE_HAS_RF 1
#endif

#ifndef DONGLE_BUILD_PROFILE
#define DONGLE_BUILD_PROFILE DONGLE_PROFILE_UNKNOWN
#endif

#ifndef DONGLE_BUILD_ID
#define DONGLE_BUILD_ID 0u
#endif

/* Terminal replies for the retired in-app flash-update commands (0x80-0x83):
 * updates now happen inside OpenBoot over OBP. */
#define DONGLE_IAP_STATUS_UNSUPPORTED 0xF8u
#define DONGLE_IAP_STATUS_NO_COMMIT   0xF9u

uint8_t dongle_nv_read(uint32_t off, void *out, uint32_t len);
uint8_t dongle_nv_is_erased(uint32_t off, uint32_t len);
uint8_t dongle_nv_erase(uint32_t off, uint32_t len);
uint8_t dongle_nv_write(uint32_t off, const void *data, uint32_t len);

/* Production status (command 0x91). The UID is the chip's eight-byte factory
 * identity; its first six bytes are the RF dongle MAC. */
uint8_t dongle_unique_id_fill(uint8_t *out, uint8_t max);

/* Fault page (command 0x93): target-specific retained fatal-fault state.
 * Returns bytes written. */
uint8_t dongle_fault_fill(uint8_t *out, uint8_t max);

/* CH570 session-AA seed entropy. ch570_capture_boot_entropy() must run as
 * main()'s first act (before the free RAM is painted/used); ch570_mix_jitter_entropy()
 * folds a weak independent clock-jitter sample in afterward, once the LSI is
 * powered. CH570-only. */
void ch570_capture_boot_entropy(void);
void ch570_mix_jitter_entropy(void);
void dongle_fault_boot(uint8_t reset_status);

#endif /* DONGLE_PLATFORM_H */
