# CH570 SRAM budget

CH570 has 12 KB of SRAM and the link enforces a 2 KB stack floor
(`CH570_STACK_FLOOR`, `link.ld`), sized to clear the 1,600-byte cold-boot
entropy window the ROM reads above `_end`. `make -C firmware ch570-ram-report`
prints where the budget goes and what is left:

```
CH570 SRAM: .highcode 8824 B  .data 96 B  .bss 1252 B
  _end 0x200027ec, free to top of RAM 2052 B, stack floor 2048 B, margin 4 B
```

**The margin is 4 bytes.** Anything that grows SRAM-resident code fails the
link, and the assert only says "less than the selected stack floor" — run the
report to get the number.

## Where it goes (measured 2026-09-13, `main` @ 476d42a)

`.highcode` — code executed from SRAM — is 87 % of the usage. `.bss` (1252 B)
is mostly hardware-mandated USB endpoint buffers (EP0-EP6, 448 B); `.data` is
96 B. So `.highcode` is the only meaningful lever, and most of it cannot move:

| bytes | what | why it must stay in SRAM |
|---|---|---|
| ~1820 | vendor `RFIP_*`, `BB_IRQHandler`, `LLE_IRQHandler`, `RFRole_Stop` | reached through the RF library's own pointers and ISRs |
| 646 | `SetSysClock` | reconfigures the clock and flash timing; cannot execute from flash while doing so |
| ~400 | `FLASH_CMD_ROM_ERASE/WRITE/VERIFY/GET_ROM_INFO` | cannot execute from flash while erasing or writing it |
| 134 | `HardFault_Handler` | must survive a fault taken during a flash operation |
| 2290 | `rf_phy_event_sink` | the RF event hot path |
| ~820 | `st_now/st_rearm/st_dispatch/st_set/st_set_at/st_cancel` | the poll-grid software timer, called from ISR context |

## Why the obvious shrink does not work

`.highcode` is a **closure**: every SRAM-resident function is reached from
another SRAM-resident function. A call from SRAM to flash needs the long call
sequence and costs extra spills around it, so moving a function out adds bytes
to its caller. A call-graph pass over the linked image found **zero** SRAM
functions whose callers are all in flash — there is no free move.

Measured example (do not repeat it): marking the diagnostics-only helper
`rf_diag_len10_reject_reason` as `DONGLE_HIGHCODE_COLD` removes its 80 B from
`.highcode`, but `rf_phy_event_sink` — its only caller — grows by 98 B. Net
**+16 B of `_end`**, i.e. margin 4 B → −12 B and the link fails. The candidate
looks ideal on paper (diagnostics, reject path only, and it already calls
`rf_accept_peer_mac`, which is `noinline`/flash-resident on CH570) and it still
loses.

The rule: a cold function is only worth moving if its callers are cold too,
otherwise the penalty lands in the hot caller.

## The one real lever

`rf_phy_event_sink` is 2290 B, 26 % of `.highcode`. Its CONNECTED poll/response
path is genuinely hot; its not-CONNECTED LEN-10 pair/beacon consumer is not, and
splitting that half into a flash-resident function would move hundreds of bytes
for a single call-site penalty. That is a refactor of the most bench-validated
function in the tree (`rf_task.c` marks its body "preserved VERBATIM"), so it
needs CH570 hardware and the pairing/reconnect oracles — it must not be done on
a CH592-only bench.
