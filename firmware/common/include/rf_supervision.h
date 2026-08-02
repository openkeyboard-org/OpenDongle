/*
 * Bridge75 stock-style 2.4 GHz supervision timing helpers.
 *
 * These are pure timing conversions recovered from the CH592 production
 * firmware: connected RX re-arms event 0x10 at conn_timeout / 20 TMOS ticks,
 * and the first post-promote supervision arm uses conn_interval / 4.
 */
#ifndef RF_SUPERVISION_H
#define RF_SUPERVISION_H

#include <stdint.h>

#include "rf_protocol.h"

#define RF_SUPERVISION_STOCK_TIMEOUT_DIVISOR 20u
#define RF_SUPERVISION_STOCK_FIRST_DIVISOR    4u
#define RF_SUPERVISION_STOCK_SCAN_TMOS     0x30u
#define RF_SUPERVISION_STOCK_WATCHDOG_TMOS 0x0d48u

static inline uint16_t rf_supervision_conn_interval_or_default(uint16_t interval)
{
    return interval ? interval : RF_PROTO_DEFAULT_INTERVAL;
}

static inline uint16_t rf_supervision_conn_timeout_or_default(uint16_t timeout)
{
    return timeout ? timeout : RF_PROTO_DEFAULT_TIMEOUT;
}

static inline uint16_t rf_supervision_stock_timeout_tmos(uint16_t timeout)
{
    uint16_t t = rf_supervision_conn_timeout_or_default(timeout);
    uint16_t ticks = (uint16_t)(t / RF_SUPERVISION_STOCK_TIMEOUT_DIVISOR);

    return ticks ? ticks : 1u;
}

static inline uint16_t rf_supervision_stock_timeout_protocol_ticks(
    uint16_t timeout)
{
    return (uint16_t)(rf_supervision_stock_timeout_tmos(timeout)
                      * RF_SUPERVISION_STOCK_TIMEOUT_DIVISOR);
}

static inline uint32_t rf_supervision_stock_timeout_protocol_ticks_margin(
    uint16_t timeout, uint16_t margin_ticks)
{
    return (uint32_t)rf_supervision_stock_timeout_protocol_ticks(timeout)
         + (uint32_t)margin_ticks;
}

static inline uint32_t rf_supervision_stock_confirmed_lapse_ticks(
    uint16_t timeout, uint16_t margin_ticks, uint8_t windows)
{
    uint32_t base =
        rf_supervision_stock_timeout_protocol_ticks_margin(timeout,
                                                          margin_ticks);
    return base * (uint32_t)(windows ? windows : 1u);
}

static inline uint32_t rf_supervision_wrapped_delta(uint32_t now,
                                                    uint32_t last,
                                                    uint32_t wrap,
                                                    uint32_t backstep_tolerance)
{
    if (now >= last) {
        return now - last;
    }

    /* The CH59x RTC32K reader can return a sample a few protocol ticks behind
     * the previous one. Treat that as zero elapsed time; otherwise a -N tick
     * backstep looks like a nearly-24-hour wrap and can poison confirmation
     * gates. True wraps have a large `last - now` span. */
    if ((last - now) <= backstep_tolerance) {
        return 0u;
    }

    return wrap - last + now;
}

static inline uint16_t rf_supervision_stock_first_tmos(uint16_t interval)
{
    uint16_t iv = rf_supervision_conn_interval_or_default(interval);
    uint16_t ticks = (uint16_t)(iv / RF_SUPERVISION_STOCK_FIRST_DIVISOR);

    return ticks ? ticks : 1u;
}

static inline uint8_t rf_supervision_stock_lapsed(uint32_t elapsed_ticks,
                                                  uint16_t timeout)
{
    return (uint8_t)(elapsed_ticks >= rf_supervision_stock_timeout_tmos(timeout));
}

static inline uint8_t rf_supervision_stock_lapsed_protocol_ticks(
    uint32_t elapsed_ticks, uint16_t timeout)
{
    return (uint8_t)(
        elapsed_ticks >= rf_supervision_stock_timeout_protocol_ticks(timeout));
}

static inline uint8_t rf_supervision_stock_lapsed_protocol_ticks_margin(
    uint32_t elapsed_ticks, uint16_t timeout, uint16_t margin_ticks)
{
    return (uint8_t)(
        elapsed_ticks >= rf_supervision_stock_timeout_protocol_ticks_margin(
                             timeout, margin_ticks));
}

static inline uint8_t rf_supervision_stock_confirmed_lapsed(
    uint32_t elapsed_ticks, uint16_t timeout, uint16_t margin_ticks,
    uint8_t windows)
{
    return (uint8_t)(
        elapsed_ticks >= rf_supervision_stock_confirmed_lapse_ticks(
                             timeout, margin_ticks, windows));
}

#endif /* RF_SUPERVISION_H */
