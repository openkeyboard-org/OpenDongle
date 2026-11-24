# CH570 SRAM budget

CH570 has 12 KB of SRAM. The application's RAM ends at `_eusrstack` = 0x20002FF0
— OpenBoot reserves the top 16 bytes (0x20002FF0..0x20002FFF) for its
boot-request word — and the link enforces a 2 KB stack floor
(`CH570_STACK_FLOOR`, `link.ld`), sized to clear the 1,600-byte cold-boot
entropy window the ROM reads above `_end`.

`make -C firmware ch570-ram-report` prints the budget, reading `_end`,
`_eusrstack` and `CH570_STACK_FLOOR` back out of the linked image so it cannot
drift from the linker's own arithmetic:

```
CH570 SRAM: .highcode 8608 B  .data 96 B  .bss 1252 B
  _end 0x20002714 .. 0x20002ff0 (_eusrstack; OpenBoot owns the 16 B above it)
  free 2268 B, stack floor 2048 B, margin 220 B
```

It exits non-zero with the figure when the margin goes negative, instead of the
link's bare "less than the selected stack floor".

## Where it goes (measured 2026-09-13)

`.highcode` — code executed from SRAM — is ~87 % of the usage. `.bss` (1252 B)
is mostly hardware-mandated USB endpoint buffers (EP0-EP6, 448 B); `.data` is
96 B. So `.highcode` is the only meaningful lever, and much of it cannot move:

| bytes | what | why it must stay in SRAM |
|---|---|---|
| ~1820 | vendor `RFIP_*`, `BB_IRQHandler`, `LLE_IRQHandler`, `RFRole_Stop` | reached through the RF library's own pointers and ISRs |
| 646 | `SetSysClock` | reconfigures the clock and flash timing; cannot execute from flash while doing so |
| ~430 | `FLASH_CMD_ROM_ERASE/WRITE/VERIFY/SW_RESET/GET_*` | cannot execute from flash while erasing or writing it |
| 134 | `HardFault_Handler` | must survive a fault taken during a flash operation |
| 2290 | `rf_phy_event_sink` | the RF event hot path |
| ~570 | `st_now`, `st_rearm`, `st_dispatch`, `st_set_at` | the poll-grid timer core, reached from the timer ISR |

## The rule for shrinking it

**A function is only free to move out of SRAM if its callers are already in
flash.** A call from SRAM into flash needs the long call sequence plus spills
around it, so moving a callee that a *SRAM* function calls adds bytes to that
caller — often more than the callee was worth. Two cases, both measured against
the same 4 B starting margin (i.e. before the `st_set`/`st_cancel` move below):

| change | function sizes | `_end` | margin |
|---|---|---|---|
| `st_set` + `st_cancel` → `DONGLE_HIGHCODE_COLD` (callers `hal_timer_arm`/`hal_timer_cancel`/`hal_event_post_delayed`/`hal_event_cancel` are all in flash) | `.highcode` 8824 → **8608 B** | −216 B | 4 → **220 B** |
| `rf_diag_len10_reject_reason` → `DONGLE_HIGHCODE_COLD` (only caller `rf_phy_event_sink` is in SRAM) | helper −80 B, caller **+98 B** = **+18 B** | +16 B | 4 → **−12 B**, link fails |

(The second row's +18 B of function size becomes a +16 B move of `_end` once the
section is re-aligned, which is what the margin follows.)

The second looks like the ideal candidate — diagnostics only, reject path only,
and it already calls `rf_accept_peer_mac`, which is `noinline`/flash-resident on
CH570 — and it still loses. Measure with `ch570-ram-report`; do not reason about
it.

Note that moving a callee whose caller is already in flash adds no new flash
dependency: that path already executed flash code immediately before.

To find candidates, disassemble the linked image and look for SRAM functions
whose call sites (`jal`/`jalr`, as opposed to merely having their address taken
for a callback table) all lie in flash.

## Remaining candidates, not taken

- `rf_send_pair_prep` (168 B), called only from `RF_TaskPump` in flash. It lives
  in the shared `common/src/rf_task.c`, so it needs a placement class that is
  cold on CH570 and `__HIGH_CODE` on CH592 rather than the existing shared
  `DONGLE_HIGHCODE_COLD`.
- `hal_dispatch_ch570_drain` (26 B), same shape, CH570-only.
- The big one: `rf_phy_event_sink` is 2290 B, 26 % of `.highcode`. Its CONNECTED
  poll/response path is genuinely hot; its not-CONNECTED LEN-10 pair/beacon
  consumer is not, and splitting that half out would move hundreds of bytes for
  a single call-site penalty. That is a refactor of a function whose body
  `rf_task.c` marks "preserved VERBATIM (it is heavily bench-validated)", so it
  needs CH570 hardware and the pairing/reconnect oracles.
