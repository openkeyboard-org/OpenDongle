/*
 * Family-name forwarding header (CH570 unified rf_task build): the shared
 * common/src/rf_task.c does `#include "hal_timing_ch592.h"` for the CH59x
 * task-dispatch shim's side entries. On CH570 those entries are thin stubs
 * over the true IRQ mux (see hal_timing_ch570.c): slot callbacks dispatch
 * directly from the TMR IRQ, so the seam cb table is registration-only and
 * the event-path dispatch entry is never reached by a live event. Goes away
 * at P6 when the seam name turns chip-neutral.
 */
#ifndef HAL_TIMING_CH592_FWD_CH570_H
#define HAL_TIMING_CH592_FWD_CH570_H

#include <stdint.h>
#include "hal_timing.h"

void hal_timing_ch592_set_cb(uint8_t slot, hal_timer_cb_t cb);
uint8_t hal_timing_ch592_dispatch(uint8_t slot);

#endif
