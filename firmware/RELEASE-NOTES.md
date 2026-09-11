# OpenDongle firmware — release notes

Firmware for the OpenDongle 2.4 GHz keyboard receiver, on WCH CH570D and CH592F
silicon. This document states the security property of the RF link, the known
issues that ship with it, and the manufacturing steps a unit needs before it
leaves the bench.

## Halt between receiver windows while the host sleeps (CH592)

With the windowed receiver of the previous entry the dongle is idle between windows, and while
the host is suspended nothing needs the core at all: the window schedule is an RTC timer, the
radio is shut and the link is down. `PM_HALT=1` stops the clock system between windows with the
vendor `LowPower_Halt`, which keeps every peripheral powered and wakes on RTC or USB (the
datasheet gives USB as a wake event for Idle and Halt only; a core in Sleep would sleep through
the host's resume and force a re-enumeration). A spike answered the one silicon question first:
56,986 halts with the radio shut, all returned, the resting keyboard served throughout, so the
BLE library needs none of the restoration a sleep taken behind the scheduler's back would.

The entry is admitted only when USB is suspended, the receiver is windowed with no window open,
no accepted window and no live link, and the next deadline is at least 5 ms away, on top of every
existing idle veto. Two independent wake sources are armed: the RTC trigger at that deadline less
a 3 ms clock-restart lead, and the RTC periodic timer at 1 s as a backstop no earlier event can
consume, so a trigger lost in the vendor prologue is recovered within one backstop period (1 s)
rather than leaving the dongle deaf until the host wakes. Because the vendor primitive takes its sleep instruction with interrupts enabled,
the entry takes a last look immediately before it and abandons the halt if the trigger has already
fired, if the deadline is within 1 ms, or if the USB state has moved (a resume or bus reset can run
after the unmask and clear its own flag, so only the driver's state proves anything). Both flags and
the pending bit are cleared on entry and exit; a stale periodic-timer flag would otherwise fire the
handler the moment it is enabled and abandon every halt. `hal_now()` is the SysTick, which stops
with the clocks, so it is advanced on the way out by the RTC delta minus the SysTick delta the awake
parts already counted. While the host sleeps the deadline cap becomes `PM_SUSPEND_CAP_US` (250 ms):
the window schedule is itself in the deadline table, so the cap only bounds the library's untracked
timers, its 1 s temperature sample and 120 s calibration.
Bench (CH592 950E9B1E, production keyboard resting, host asleep, measured on the dongle's 3V3
feed): 1.347 mA against 10.13 mA before Tier 2 and 5.74 mA awake, halts averaging 136 ms with zero abandoned entries, 393 catches on 395
phase windows, one give-up, no reset. Awake behaviour unchanged: poll replies 1139-1140/s of
1143/s sent, fresh pair 2/2, reacquire cadence 33.4/s, bonded reconnect 10/10 at 5 ms, the 5- and
10-slot hop gaps held, and windowing never engages against a keyboard that holds its link.
That is an operating point, not a compliance claim: it is the 3V3 rail rather than VBUS, one
keyboard resting, and neither the unbonded camp nor a continuously connected keyboard windows at
all. Shipped opt-in until its gates were complete; `PM_HALT` now defaults on (2026-09-11), and the
default image is byte-identical to the one every gate below ran against, build id E514E4F0. Setting
`PM_HALT=0` reproduces the pre-R3b image byte for byte with the build id forced. Four of the five
gates ran on the merged code (E514E4F0, production keyboard resting). A 47-minute host sleep: the
clock advanced 2843.6 s of 2845.6 s of wall time; the device resumed with its USB node identity
unchanged rather than re-enumerating; and the soak recorded 14,049 halts totalling 1917.6 s, 67 %
of the wall clock, with zero abandoned entries, 1,622 reconnects at about one a second, 1,366
catches on 1,368 phase windows, two misses, one give-up and no reset. Separately, the USB data
cable was pulled and replaced while the host slept and the dongle was halting, with the debug probe
still powering the board: the boot count did not move, there was no watchdog recovery, the device
enumerated fresh as a replug should, and the link returned in a second. The fifth gate, a remote
wake driven out of a halt, has since run too (2026-09-11). A Nucleo-U083RC carrying the
OpenController driver stands in for the keyboard MCU on the module's UART, so a keystroke can
originate while the host is asleep and every process on it is frozen. It parks on the USB host at
reset, which drops the 2.4 GHz link and puts the dongle in its bonded camp, where the windowed
receiver engages on the grace rather than on the scheduled-drop detector; it then returns to the
wireless host and presses one key. The host slept at 12:09:35 and woke itself at 12:12:00, 145 s
later, with the wake attributed to `USB2_wake` rather than to the internal keyboard. Across that
episode the dongle logged 330 halts totalling 47.4 s with zero abandoned entries, the stand-in sent
exactly one keystroke, the USB node identity was unchanged, the clock advanced 224.6 s of 224.7 s
of wall time, and the boot count did not move. The halting state there was the camp rather than a
resting production keyboard, but the halt entry, the radio wake and the remote-wake pulse are the
same code on both paths. That emptied the gate list, and the knob was defaulted on the same day.
The entry also carries a known residual: the vendor primitive takes its wait with interrupts
enabled, so a resume arriving inside its prologue is serviced and then slept through, bounded by
the backstop at one second. With the knob off the image is byte-identical and
CH570 is untouched.

## Windowed receiver in the reacquire scan and the terminal camp (CH592)

Against a resting production keyboard the dongle spent ~990 ms of every second in the
reacquire scan to catch a ~10 ms probe that arrives once a second (the keyboard stops its
session 5 s after the last key and reconnects every 1.010 s), and against an absent
keyboard the camp kept the receiver on for nothing; the receiver is about 7 mA of the
dongle's draw. The scan and the camp now open short receiver windows instead: 30 ms on
channel 8 every 200 ms (the production probe sweeps all three pairing channels inside its
~10 ms, so one channel suffices), plus one window a second phase-locked to the resting
keyboard's next probe once a detector has seen two scheduled drops (a short confirmed link
dropped within 300 ms of its promote, ~1 s apart); the radio is shut between windows. A
missed phase window widens the next one and two misses fall back to the continuous scan
until the next promote, restoring continuous reception before the keyboard's third
unanswered probe (which would send it to a sleep stage only its host can end). Acceptance inside a window hands the
radio to the existing accept path untouched. A grace period keeps today's continuous
receiver for a nominal 60 s after a scheduled drop (counted 1.010 s per resting cycle in the
scan and 200 ms per tick in the camp, replenished by an unscheduled loss), so the first key
after a short pause keeps its ~40 ms; the unbonded fresh-pair camp and the 3 s boot window stay continuous.
Knobs `PM_RX_WINDOW` (0 is byte-identical to before), `PM_RX_WINDOW_MS`, `PM_RX_PERIOD_MS`,
`PM_RX_GRACE_S`, `PM_RX_PHASE_MS`; diag page 7 `rx window` counts opens, closes, catches,
phase opens and misses, detector locks and the reasons a drop did not qualify.
Bench (CH592 6308E6D8, production keyboard, grace 0): 297 resting cycles, 297 phase
windows, 297 catches, 0 misses, 0 give-ups, 0 fallbacks; first key after rest 20/20 within
48 ms (40 ms before); cold reconnect at random phases 10/10 within 199 ms; the awake resting
state metered 5.74 mA against 10.67 mA (the same keyboard, one session's scale). With the
build id forced, CH570 and the knob-off CH592 image are byte-identical to before (ordinary
images differ in their identity bytes, since the id hashes the shared sources and flags). Suspend-state core Halt between
windows is the next step; the awake floor with the receiver off is 3.06 mA.

## Connected hop counts the keyboard's edges

The connected data-hop reproduced the recovered dongle rule: reset the anchor to
the poll time, step by elapsed / interval, and when the index landed back on the
previous one force a slot forward and forward-date the anchor. That repeat-correction
is right for a poll a tick early and wrong after a coalesced gap of exactly 5k poll
slots, where the keyboard is already on that channel: the forced slot misses it, the
next poll hops two, and supervision drops the link. Reproduced on the bench by masking
the dongle's interrupts for whole poll slots on a live link (`opendongle --rf-poke
40+N`): 5 and 10 slots dropped the link every time, 1-4, 6-8 and 12 never. The
dongle now keeps the keyboard's model instead: an edge anchor 13 ticks before the
poll it expects, seeded at every re-key, advanced by whole intervals only, the index
by the same count, no correction branch; the modular add subtracts before it adds,
which also closes the 32-bit overflow the old branch had (TODO). Both ends count the
same edges however the polls are spaced. A host test (`firmware/tests/test_hop_model.py`)
pins the model against the keyboard's for every gap of 0..20 slots and 0..27 ticks.
Bench (CH592, 664BA4F5, OpenController as the keyboard): masked gaps of 1..15 slots
and the 5- and 6-slot remainder bands all held the link with 0 lapses; 30 alternating
5/10-slot gaps, 0 lapses; EV10 cadence, reconnect 10/10 at 5 ms, fresh pair and the
poll reply ratio unchanged. Both chips' bytes change; CH570 compiled, not bench-verified.

## EP6 OUT stays live after a dropped IAP packet

The vendor HID interface's OUT endpoint was NAKed unconditionally after every OUT
completion, while only a latched IAP command ever re-ACKed it; a toggle mismatch with
nothing pending therefore wedged the maintenance interface until a bus reset or a power
cycle. That case is reachable by a conforming host: a valid OUT whose ACK is lost on the
bus is retried with the same DATA PID after the command has already run (USB 2.0 8.6.4).
The handler now re-ACKs when nothing was latched and nothing is pending, judging the
toggle from the interrupt-status sample it already took; a latched command keeps the
flow-control NAK that `USB_PollEP6` releases. Not reproducible from the bench host, so
verified as non-regression (pipelined writes answered in order, the maintenance flows
unchanged). Both chips' bytes change; CH570 compiled, not bench-verified.

## Terminal-camp liveness watchdog

Every terminal camp (EV10 give-up, closed boot window, unbonded start) armed RX once
and then leaned on the radio's own events to re-drive it; the arm-status guard that
was meant as the backstop never fires on either radio library, and CH59x basic-mode
RX has no timeout at all, so a lost completion or a deaf PHY left the dongle in
`waiting for reconnect` until a chip reset. The camp now keeps the boot-window timer
slot, free in every terminal camp, as a 200 ms liveness tick. A tick that saw an
RX-side event since the last one leaves the radio alone; a silent tick re-arms RX
from task context (CH59x: every silent tick, since a silent camp is the normal state
there; CH570: the 30 ms camp timeout keeps the counter moving, so a silent tick is a
dead loop and the second in a row escalates to shut + vendor re-init + re-arm, rung 2
of the 0x94 ladder's code path, not bench-verified on CH570 here). The tick self-disables outside the exact terminal camp, and a
restart that lands in a terminal camp without it (the bond-clear tombstone paths) starts
it. The same pass fixed the latent CH570 persist ordering (TODO): the record is validated
before the radio is torn down, not after.
`opendongle --rf-poke 3` is the fault injection (shut the radio, leave it deaf) and
page 1 `camp_wd_rearms` counts the re-arms. Bench (CH592, 4AB7E07F): 5.0 re-arms/s in
the silent keyboard-absent camp; five rung-3 fault injections each read `rx_armed`
false right after and true 0.5 s later, then reconnected in 5 ms, and one left deaf for
1.5 s reconnected the same; three induced EV10 scans at 71 PAIR_PREP runs each
(`hb_stale_drop` 0); bonded reconnect 10/10; three fresh pairs through the unbonded
camp (bond clear, cold boot, factory pair) at 69-71 ms to connected with two pair ACKs
and one bond persist each. CH570 compiles; not bench-verified (no CH570 on this bench).
Bounds: a lost completion is re-armed within two ticks (the tick after the loss may
still read the event that preceded it); rung 3 resets the baseline and is re-armed at
the next tick. Not covered: a CH570 PHY that keeps raising its 30 ms timeouts while
receiving nothing looks alive to this test (TODO).

## Link order pins the CH592 radio path

The poll reply ratio (replies received over the keyboard's replies sent, the page-1
oracle) moved by up to 0.3 % between builds that changed nothing on the poll path:
a `volatile` on a diagnostics block, a comment-sized edit to the idle code. The map
explained it: the RAM-code image was loaded ahead of flash `.text`, so every RAM-code
edit shifted the whole BLE library, and the library's sections were linked after every
application function, so every application edit shifted them too; the poll path runs
mostly from flash (`RF_Rx`/`RF_Tx`/`RF_Shut`, the library's receive and transmit
processing, TMOS, and the application's dispatch and poll functions). `link.ld` now
links the radio path first, the BLE library and then the application's flash-resident
poll-path functions, and loads the RAM-code image after `.text`. The library's
addresses are then identical across RAM-code and application perturbations (`RF_Rx`
at 0x3cee in every variant), and the pinned application functions move only when a
function ahead of them in that list changes. The image is 68 bytes smaller: the
linker relaxes 22 library-internal calls to `c.jal` from the new proximity, which is
also why the byte-identity gates do not apply across this change; the gate for a
link-order change is the symbol set with sizes (identical) plus the bench oracles.
`TEXT_PAD=N` (Makefile, default 0, N even) shifts flash `.text` by N bytes for
placement experiments, rounded up to the first pinned section's alignment (read
`RF_Rx` in the map); the flash-fetch period behind the effect is not established.

## Power management (CH592, Tier 1): main-loop idle, GPIO park, clock gates

The CH592 product build no longer busy-spins its 60 MHz core. `Main_Circulation` now
idles with a SEVONPEND/WFITOWFE wait under the global mask (the form OpenController
proved on the same silicon; a plain masked WFI never wakes here), bounded by TMR3 at
the lowest interrupt priority, and woken by the radio (BLEB/BLEL), TMR0, USB and TMR3.
TMR3 is armed per sleep to the next application TMOS deadline (`PM_EXACT_DEADLINE=1`,
the default): the CH592 timing seam records every application timer start in a
table on the RTC counter TMOS itself compares against, retires an entry once a sleep
decision finds it already due at the previous decision's clock read with nothing
dispatched since (the scheduler passes between two decisions dispatch every expiry
TMOS had posted), and the idle path programs TMR3 to fire one 625 us unit after
the nearest entry, or at `PM_DEADLINE_CAP_US` (20 ms) when none is nearer, which
bounds the library's own timers (its 1 s temperature sample, the 120 s calibration).
An application timer therefore waits at most one TMOS unit of idle-induced wake
latency (plus the foreground and scheduler passes it always ran behind), a library
timer at most the cap plus the same passes, and the heartbeat interrupt runs about fifty times a second
instead of a thousand when no application deadline is nearer than the cap (an
application timer due sooner arms it sooner; on a live link TMR0 wakes the core
first and the heartbeat hardly fires at all). `PM_EXACT_DEADLINE=0` restores the fixed 1 ms period
(`PM_HEARTBEAT_US`) byte for byte. Measured back to back on the devboard (inline
meter on the probe rail, same hour, 60 s averages): keyboard absent with the host
asleep, the overnight state, 9.910 -> 9.773 mA (-0.14 mA, the heartbeat ran at
~50/s instead of 1000/s through the sleep); keyboard absent with the host awake
10.907 -> 10.842 mA; connected, awake 10.836 -> 10.83 mA and connected, host
asleep 10.694 -> 10.716 mA (equal within noise: there TMR0's 875 us poll cycle
wakes the core whatever the heartbeat does).
The radio is never slept, the protocol bytes and timer settings are unchanged (the
measured poll cadence stays within 1 % of baseline), the flash stays powered, USB
suspend and remote wake keep working, and the DC-DC is never enabled (the board has no
inductor). Build knobs (`firmware/ch592/Makefile`, all `-D` flags hashed into the
build id and named in `CONFIG_TEXT`, schema 12 as of the exact-deadline change): `PM_IDLE` (1), `PM_IDLE_LEVEL`
(3 = idle in every RF state: 1 = keyboard-absent camp only, 2 = every pairing
sub-mode + idle), `PM_IDLE_IN_SUSPEND` (1), `PM_GPIO_PARK` (1), `PM_CLK_GATE` (1, see
below) with `PM_CLK_GATE_MASK` (19958 = 0x4DF6), `PM_USB_DIGIN_OFF` (0),
`PM_HEARTBEAT_US` (1000), `PM_EP0_QUIET_MS` (200). Building with the seven switches at
zero (`PM_IDLE=0 PM_IDLE_LEVEL=0 PM_IDLE_IN_SUSPEND=0 PM_GPIO_PARK=0
PM_USB_DIGIN_OFF=0 PM_CLK_GATE=0 PM_RX_WINDOW=0`; the period and mask knobs keep their
defaults; `PM_RX_WINDOW` joined the set with the Tier 2 windowed receiver)
produces an image byte-identical to the pre-change firmware (verified with a forced
build id), and the CH570 image is untouched. New IAP 0x92 pages 5/6 ("power")
expose idle duty, wake attribution (radio / TMR0 / TMR3 / USB), the veto histogram,
stale-interrupt clears and the clock-gate word; `opendongle --diag` decodes them.

Bench ladder, 2026-09-06, WeAct CH592F devboard, meter inline on the probe's 3V3 feed
(the board's only supply, so whole-dongle current), 90 s min/avg/max per reading,
every rung compared against a same-day all-off baseline because the bench had
shifted 0.75 mA overnight:

| Scenario | all-off baseline | plumbing (level 0) | + GPIO park | idle level 1 | idle level 3 | + clock gates (shipped) |
|---|---:|---:|---:|---:|---:|---:|
| Connected, idle | 13.26 | 13.55 | 13.03 | 13.16 | **10.87 (-18%)** | 10.62 |
| Connected, ~9 keystrokes/s | = idle | | | | 10.93 | |
| Bonded, keyboard absent (search) | 15.90 | 15.64 | 15.68 | **10.41 (-35%)** | 10.39 | 10.13 |
| Host USB suspended, link up | 13.23 (day 1) | | | | 10.71 | |

What the rungs taught: the heartbeat plus the per-pass idle logic cost +0.29 mA while
the core still spins, and a later A/B/C in the keyboard-absent camp (1 ms heartbeat
10.931 mA, 10 ms 10.819 mA, none at all 10.796 mA) showed that cost is the interrupt
itself, about 0.13 mA per thousand a second, while TMR3's 60 MHz counter is free, which
is why the exact-deadline mode keeps the timer and only stops firing it periodically; the GPIO park is worth -0.52 mA on the
devboard (floating header pads); idle removes the core-spin share in every state;
keystrokes cost nothing measurable. Per-rung gates (all pass on the shipped build):
poll rate one per 875 us within 1% of baseline, 0 supervision lapses / EV10 entries /
reboots over a 10 min connected soak, bonded reconnect from the idle camp 10/10 at a
55 ms median (baseline 52), fresh pair 10/10 with the durable bond persisted, LED
relay 10/10 end to end, USB re-enumeration 40/40 with the link re-formed, bootloader
entry from a live link, USB suspend with remote wake armed, wake attribution proving
the radio interrupt ends the masked idle (0 radio wakes with the keyboard absent).

Findings folded in during validation:
- The BLE library's 1 s temperature sample leaves the read-only ADC EOC flag set, so
  IRQ 29 reads pending forever and the strict alien-interrupt veto held idle at 0 %
  until `pm_irq_pending` learned to clear it (page 6 `stale_adc`).
- Idling during the fresh-pair ACK burst / confirm-before-persist phase lost the
  confirming packet on 3 of 15 pairs (durable bond write deferred) against 0 of 10 on a
  never-idle control; those phases now veto idle at every level (they last < 1 s).
- The three OS-polled HID IN endpoints (1 ms interval) wake the core ~3000/s while the
  host is awake (NAKed IN tokens pulse the USB IRQ), capping awake idle duty near 86 %
  in the camp and ~38 % on a live link; gone during suspend.
- Clock gating (`PWR_PeriphClkCfg(DISABLE, 0x4DF6)`: TMR1/2, UART0-3, SPI0, PWMX, I2C,
  LCD) saves a further ~0.25 mA in every state and ships on by default (`PM_CLK_GATE=1`,
  mask knob `PM_CLK_GATE_MASK`). It first shipped opt-in because a morning A/B on the
  absolute receive-restart rate read 1.6 % lower with the gates on and an afternoon
  bisect could not name a block. Re-measured 2026-09-09 with the page-1 reply-ratio
  oracle (replies received over the keyboard's replies sent, 30 s runs, interleaved
  flashes): the gates cost about 0.2 % of poll replies, not 1.6 % (the old oracle
  counted the downlink's own loss too, which is the likely rest), and the same binary
  with an empty mask reads like the ungated one, so the register write, not code
  layout, carries it; each half of the mask (TMR1/2 + UARTs, and SPI0/PWMX/I2C/LCD)
  carries about 0.15 % on its own, so no single block explains it. The mechanism is
  not established; a supply effect of the gated current is the guess. A missed reply costs the
  keyboard one 875 us poll of latency on that report; the 0.25 mA is 2.3 % of the
  connected draw. `PM_CLK_GATE=0` restores the ungated image.
- On this Mac the CH592 OpenBoot USB bootloader attaches but never binds as an HID
  device, so `opendongle --enter-bootloader` + `openboot flash` cannot update the
  CH592 dongle from macOS; every rung was flashed over SWD (`make ch592-factory-flash
  ... ALLOW_BONDED_FLASH=1`, the bond survives `-E`). Recorded as a separate bug.

## Boot architecture: OpenBoot

The dongle boots via the [OpenBoot](../third_party/openboot) bootloader (pinned
submodule): bootloader at `[0x0000,0x2000)`, application at `0x2000`, a boot
record in the top erase block of each A/B slot, and updates performed **inside
the bootloader** over its OBP USB protocol. The bootloader re-enumerates under
the dongle's own `0C45:FEFE` identity, told apart from the application by its
vendor HID usage page `0xFF00` (the application uses `0xFFFF` and `0xFF60`), and
the flash returns it to the application. See [BOOT.md](BOOT.md) for the flash maps, the
boot-record semantics, the update/factory/recovery flows, and the honest caveats
(fail-stay after an abandoned session; wrong-family protection is host-side).

Behavioural properties worth knowing:

- Two different interruptions have two different outcomes; they are not in
  conflict, but they are easy to confuse:
  - **The transfer dies** (host crashes, cable pulled, power cut mid-write).
    The unit comes back **running the previous application**: mutations target
    the inactive slot, so the running image is never touched and BOOT falls
    back to it. Retry the flash.
  - **A session is opened and then abandoned** (a successful HELLO, then the
    host walks away without flashing or sending BOOT). The unit **stays in the
    bootloader** — a HELLO suppresses idle auto-boot until reset, which is the
    fail-stay rule. Power-cycle it, or end scripts with an explicit
    `openboot boot`.
- A factory image carries slot A's boot record, so a blank part boots the
  application on first power-on with no host and no bless step. On a blessed
  unit `openboot bless` is not merely unnecessary but impossible: the device is
  `active=A, write=B`, and bless resolves against the write slot.
- What the boot-time CRC proves is narrower than it looks: the record stores a
  length and a CRC over that many bytes from the slot base, so it is protection
  against accidental corruption, not an identity check. `BOOT.md` states the
  exact rule.
- `OB_IDLE_TIMEOUT_MS` is now **real milliseconds**. The boards' unchanged
  `10000` means 10 seconds, where it previously bench-measured ~273 s on CH570
  and ~86 s on CH592 — the value did not move, its meaning did. Anything that
  waited out the old window is ~27x too slow.
- Finish an SWD factory flash with a real power cycle, not `minichlink -b`:
  `-b` resumes the core instead of resetting through the boot path, so the boot
  decision never re-runs from a cold start. `flash-factory` does this for you.
  Established by controlled comparison on a CH570 — same part, same bytes, same
  SWD path, only the final step differing: with `-b` nothing enumerated at all,
  with a real power cycle the application came up and ran.
- Hardware validation after the A/B adoption, on a CH570 taken from blank
  silicon to a working dongle: bootloader and application USB enumeration under
  `0C45:FEFE` with usage-page disambiguation, OBP 0.2, on-silicon slot geometry
  and the bond clamp (`write window 0x2000..0x1D000` as reported by the device
  itself), the dry-run/`--force` flash path, `--enter-bootloader`, the 10 s
  idle auto-boot, and factory-blessed first-power-on boot. Product level:
  pairing, typing, media keys, indicator LEDs, reconnect and sleep/wake.
  CH592 covers the same protocol and geometry over UART plus a verified COMMIT.
- **The A->B slot transition is validated on hardware** as of the dual-slot
  build. A CH570 was taken through a full A->B->A round trip over USB with the
  real application, using a `.obb` bundle:

  | step | device reported | bundle selected |
  |---|---|---|
  | start | `active A, writing B`, window `0x1E000..0x39000` | slot B, crc32 `0x893FE89B` |
  | after | `active B, writing A`, window `0x2000..0x1D000` | slot A, crc32 `0xD7958914` |
  | after | `active A, writing B` | — |

  The running build id matched the selected slot's image each time
  (`0x44126E29` for slot B, `0x9F666DEE` for slot A), so the variant really was
  chosen by the device's `write_base` rather than assumed. **The RF bond
  survived both updates** - it sits at `0x3A000`, which is exactly
  `OB_APP_END`. That bound is exclusive, so the writable region is
  `[0x2000, 0x3A000)` and the bond is the first address outside it; OBP cannot
  reach it.
- **Still not validated on hardware: the interrupted-update paths.** Upstream's
  bench harness reaches the bootloader through a CDC-open target reset that this
  bench does not exhibit. That is a limitation of OpenBoot's test code, not the
  product: a source audit of `firmware/core/`, `ports/` and `transports/`
  against every hardware observation found no bootloader defect.

## Pair-acceptance RSSI floor: -75 -> -90 dBm

The fresh-pair path (and a boot-window accept of a *different* keyboard) is
gated on received signal strength. A bonded keyboard reconnecting is **not**
gated, so this only ever affected pairing.

**-75 had negative margin at the distance the product is for.** Bisected with
diagnostic builds against stock v0.96.15 with a bonded link: -128 accepted,
-90 accepted, -82 accepted, **-75 rejected**. The keyboard's pair broadcast
therefore arrives at roughly -81..-76 dBm with both boards on one bench, so a
dongle behind a PC case or across a desk sits in that band or below — the
shipped default was rejecting its own primary scenario.

**-90 keeps what the gate is for.** Its purpose is stopping an unprovisioned
dongle auto-pairing with a distant keyboard in a dense environment. At 2.4 GHz
a cross-room signal (several metres plus a wall) generally lands below -90
while same-room stays above, so this keeps the "not the neighbour's office"
property with ~10 dB of margin over what was measured.

Two things to be honest about:

- **The bracket is bench-specific.** It reflects one geometry and one pair of
  boards. Before this ships, re-measure at the intended worst case — dongle on
  a rear I/O port, keyboard at arm's length. If that lands below -90, prefer
  -95 over deleting the gate.
- **The gate is a heuristic, not a security boundary.** CH59x RSSI is coarse
  and uncalibrated chip to chip, so the same number means different real
  distances on different units, and the CH570 SKU is reported to run with it
  effectively inert (constant RSSI byte) — the product line already tolerates a
  no-gate configuration.

If mispairing in dense environments ever becomes a real complaint, the fix is
**not** a tighter floor: it is strongest-candidate selection — briefly collect
broadcasters during pairing and take the highest RSSI, keeping this only as a
sanity floor. An absolute threshold encodes antenna and geometry assumptions;
relative selection targets the actual failure mode.

Two supporting changes ship with it. `opendongle --info` now reports the last
RSSI the RF task saw, so re-measuring needs one command rather than a bisect
over four diagnostic builds; and the floor is overridable per build
(`make -C firmware ch592 PAIR_MIN_RSSI=-95`, or `ch570`) so bench profiles do
not need a source edit.

**This does not address the keyboard-reset-recovery regression** (EV10
reacquire gating out rebooted keyboards). That is a separate v0.96.x fix, and
both want the same bench re-validation pass.

## Bonded reconnect vs the OpenController MR4 receive window

A bonded CH570 dongle that has not connected since it booted did not complete
the OpenController keyboard's bonded reconnect, and neither did one that had
settled into its channel-8 camp after an EV10 give-up on the first try. The
keyboard side saw ~500 clean beacons in 10 s and zero valid frames; the dongle
side heard and accepted every beacon it caught, transmitted its LEN-15 reply,
promoted to CONNECTED, lapsed for want of a poll response and relistened,
indefinitely. A fresh pair recovered it; nothing else did. Bench 2026-09-04,
three consecutive cold boots, measured with the RF diagnostics page proposed in a separate draft PR.

**Mechanism.** The reconnect reply is scheduled `RF_CH570_PAIR_ACK_PRE_TX_TMOS`
= 4 x 625 us after the beacon (software-measured: 2538..2551 us from accept
to StartTx returning, ~214 us from there to the TX-finish callback), the
value chosen for the Bridge75 stock keyboard. The OpenController's power-MR4
bonded search arms its receiver ~0.2 ms after each beacon and closed it
2.5 ms later -- consistent with the reply completing on that closing edge.
Its window had been sized against the dongle's EV10 reacquire path, whose
reply comes ~0.3 ms after the beacon and which a dongle only takes once it
has connected at least once since boot -- so every earlier measurement, made
against a warm dongle, passed.

**Resolution.** On the keyboard side: OpenController widens the window to
6 ticks (3.75 ms), leaving ~1 ms of completion margin. This dongle's timing
is unchanged; the bound the keyboard must respect is now documented at the
constant. The dongle-side alternative (reply at 1.875 ms) was bench-proven
too (6/6 cold-boot reconnects, 5/5 after a give-up) and not taken, because
the source records it as measurably worse for the Bridge75.

**Two things to be honest about.**

- The 0.2 ms keyboard arm latency is an estimate; what is established is the
  A/B: a 4-tick window passes 0/8 (fails 8/8) and a 6-tick window passes, and a 1.875 ms
  reply passes against a 4-tick window. The exact cross-device edge was not
  measured with a sniffer.
- A second failure of a different shape was seen once on the same day and is
  NOT covered by this: a dongle up for two days answered neither keyboard
  build (including a continuous-RX build that catches the reply above) and was
  cured only by a power cycle. It predates the diagnostics page and is
  uncharacterised; `TODO.md` records the structural hole it matches (the
  terminal camp has no time-based liveness backstop). The RF diagnostics page
  and intervention ladder in the separate draft PR exist to characterise it
  when it recurs.
## Diagnostics: RF page (IAP `0x92`) and intervention ladder (`0x94`)

Page 1 (PHY counters: RX arms and failures, RX done / CRC error / timeout, TX
start / fail / done, shut calls, last status bytes) is now live on CH592 as well;
it used to read as zeros there. The `tx_done` / `rx_done` pair is the poll
reply-rate oracle: on CH592 a missed reply raises no event, so it shows only as
`rx_done` falling behind `tx_done`.

`opendongle --diag` reads five 62-byte pages over the vendor HID interface
without arming a maintenance session: the runtime snapshot (state, channel,
the access address in RAM against the one the radio was last armed with,
last RX/TX/shut status, an RX-armed latch, peer MAC, last beacon disposition,
last persist outcome), the PHY and executor counters (RX arm attempts and
failures, RX done/CRC/timeout, TX start/fail/done, the pending event mask,
both delayed-post slots, every timer slot's remaining time), the protocol
counters (beacons seen and accepted or rejected with the reason, reply
scheduled/started/finished, EV10 entries and give-ups, promotes, relistens,
confirm and persist outcomes, reply latency) and the radio internals (LLE/BB
interrupt counts, a free-running SysTick timebase with stamps, the LLE/BB
registers read through the vendor library's own base pointers, its receive
state, the radio IRQ enable bits, USB suspend episodes). Counters wrap; the
tool prints per-second rates between samples. A healthy reconnect camp reads
as RX re-armed ~33/s on its 30 ms timeout with the radio address equal to
the bond's; a deaf one is told apart by which of those stops.

`opendongle --rf-poke 1|2` (armed) re-arms the receiver from task context, or
shuts, re-runs the vendor init and re-arms. It is refused unless the dongle is
in its exact terminal camp, and it exists to tell a dead software loop from a
deaf PHY in place, without a debug probe (which on CH570 shares the USB pins).

Honest limits: the counters are best-effort (no locking; a sample may straddle
an event); the LLE/BB register meanings come from the linked library's
disassembly, not from documentation; the SysTick stamps wrap every 42.9 s;
`0x92` costs one EP6 exchange per page, which stalls the RF pump for
milliseconds, so it is not for use inside a latency measurement. Footprint on
CH570: ~2.9 KB flash, ~0.6 KB RAM, within the 0x800 stack floor.

## Security property: the RF link provides no confidentiality

The Bridge75 2.4 GHz data path applies **no confidentiality protection**, by
design and for wire compatibility with the production keyboard. Specifically:

- The per-session access address is sent in **cleartext** in the pair-ACK on the
  well-known pairing address.
- The 5-channel frequency-hop schedule follows **deterministically** from a known
  constant.
- HID reports travel **verbatim** on the link (`rf_protocol.h`, `rf_task.c`).

The prebuilt vendor BLE archive contains cryptographic and SMP routines, but this
product uses raw `RF_Rx`/`RF_Tx` and never invokes them. An attacker within radio
range can recover keystrokes. This is a property of the wire protocol, not a
defect to be fixed in this firmware — fixing it would break interoperability with
the production keyboard. It is stated here so it is an explicit, accepted property
rather than an implicit one.

## USB suspend: mouse and consumer reports no longer replay on resume

The suspend handler used to NAK only the boot-keyboard endpoint (EP1) when the
host stopped SOF. A mouse (EP2) or consumer/media (EP3) report armed in the last
poll interval before suspend stayed armed across the whole episode and was
handed to the host on its first IN poll after resume. HID reports are
change-driven, so the matching release, arriving over RF during suspend, was
correctly dropped while the press survived: the host resumed with a stuck button
or a held media key until the next real report from that device. The suspend
branch now NAKs EP1, EP2 and EP3 alike, masking only the response bits so the
data toggle is preserved (the host never received the NAK'd report, so its
expected toggle is unchanged). The NAK alone would only trade the glitch for its
mirror image (a *release* armed when SOF stopped would now be the report that
never arrives), so on the resume edge the main loop also sends a mouse all-up
report with zero motion and a consumer no-key report, the same reconciliation
the keyboard gets from its keys-up flush; the next real report re-asserts
whatever is still held. A fresh report that reaches the endpoint first, in the
window between the resume interrupt and the main loop, cancels the flush for
that endpoint (it carries the device's whole current state), so a press that
lands right at resume is neither dropped nor released by the flush; the keyboard's
own resume delivery (the stashed waking keystroke, or keys-up) is gated the same
way, so a report armed after resume outranks the stash. On Linux the
no-key report also resets the retained consumer report, which would otherwise
suppress the next identical press; the one residual is a fresh consumer report
of that same usage arriving inside the resume window, in which case the host
never sees the no-key report and that press is suppressed until any other
consumer report arrives. Mouse and consumer
reports remain drop-on-suspend (no stash); only the keyboard's waking keystroke
is preserved. Changes the image on both chips.

## Known issues

**CH592: rare fatal fault under a reset during BLE activity.** Under a debug
attach (a halt-and-reset) while the BLE stack is active, the CH592 can take a
fatal fault inside the vendor TMOS layer (`mcause` load fault/misalign, `mepc` in
`TMOS_SystemProcess`/`tmos_proces_system_time`). It is characterised but not
root-caused, and it lives in the prebuilt BLE archive, not in OpenDongle code.
Accepted because:

- the only observed trigger is a reset landing during BLE activity, which a
  **debug attach** produces and a production unit — which has no debug access —
  cannot;
- plain power-rail cycling is clean (10/10 in the characterisation run, and a
  50-cycle soak on the shipped configuration);
- the fault **self-recovers** through the firmware's one-permitted-software-reset
  path (`last reset software`, `boot count` advances, `action recovered`), and a
  real power cycle clears the retained record entirely.

Repeated events within a single power cycle can still reach the fault handler's
repeat fail-stop state rather than recovering; a power cycle clears it. The
application's own reboot-into-bootloader path (vendor HID command `0x85`)
quiesces the radio before resetting, specifically to keep that reset away from
live BLE activity.

## Manufacturing / provisioning duties

**Clear the bond before shipping a CH592 unit.** The CH592 factory flash cannot
erase the bond — it lives in DataFlash, which the probe's whole-chip erase does
not reach — so `make ch592-factory-flash` **fails** if a `BOND` record survives,
and points you at clearing it over the vendor HID interface (command `0x89`). A
shipped unit must not carry a test fixture's pairing identity.
`ALLOW_BONDED_FLASH=1` overrides the check for a deliberate reflash of a bonded
development unit. On CH570 the bond is in code flash and the factory erase clears
it, so a factory-flashed CH570 always needs a fresh pairing.

**Recovery is USB ISP or the bootloader on production hardware.** SWD recovery
was validated on the devboard only. On the production keyboard `PB15` is tied to
GND (it is also TCK), so two-wire debug is unavailable; a stuck production unit is
recovered via USB ISP (`wchisp`) or, if the application still answers, by
rebooting into OpenBoot and flashing from there:

```sh
opendongle --enter-bootloader --image <slot-a>.bin
openboot --vid 0x0C45 --pid 0xFEFE flash <chip>-product.obb --force
```

**Flash the bundle, not a bare `.bin`.** Under A/B the device refuses an image
whose base is not its current `write_base`, and which slot that is depends on
how many times the unit has been updated — so a bare `.bin` is right only by
luck. The `.obb` carries both per-slot builds and the CLI picks the matching
one. (`--image` on the first command still takes a plain `.bin`: it is only
read host-side for the family guard, never flashed.)

Naming the image on the first command is what engages the wrong-family guard,
which compares the image's ODG2 family against the running application before
the reboot — the only point at which that check is possible. Plain
`opendongle --enter-bootloader` is also accepted and reboots the unit, but with
no image to compare it performs no family check.

On CH570 specifically, the debug pins *are* the USB pins: a USB-attached device
has no SWD, and an SWD session is usable only between a whole-chip erase and the
first boot.
