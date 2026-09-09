// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! Decoder and display for the read-only RF diagnostics pages (command 0x92).
//!
//! The firmware side is `RF_DiagFill` in firmware/common/src/rf_task.c; the
//! page layouts are documented there and mirrored here field by field. Every
//! page is 62 bytes, little-endian, byte [0] = page version, [1] = page id.
//! Counters wrap; `--samples` prints per-second rates between samples.

use anyhow::{bail, Result};

use crate::iap::{hexsp, op_rf_diag, IapDevice};

pub const ACK_RF_DIAG: u8 = 0x92;
pub const PAGE_LEN: usize = 62;
/// Pages 0-3 are required of any firmware that answers 0x92; page 4 is present
/// on the current firmware of both chips but tolerated missing (an earlier
/// firmware answers it with an empty payload and it is skipped, see
/// `REQUIRED_PAGES`); 5 ("power") and 6 ("power detail") exist only on CH592
/// firmware built with PM_IDLE=1 and are skipped otherwise.
pub const PAGE_COUNT: u8 = 7;
const PAGE_VERSION: u8 = 1;

/// Little-endian u16 at `at`.
fn le16(p: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([p[at], p[at + 1]])
}
/// Little-endian u32 at `at`.
fn le32(p: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([p[at], p[at + 1], p[at + 2], p[at + 3]])
}

/// One fetched page: validated framing, raw payload kept for printing.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Page {
    pub id: u8,
    pub raw: [u8; PAGE_LEN],
}

impl Page {
    /// Decode and frame-check a response; errors name the offending byte.
    pub fn decode(expected_page: u8, response: &[u8]) -> Result<Self> {
        if response.len() < 2 {
            bail!("RfDiag: short response ({} bytes)", response.len());
        }
        if response[0] != ACK_RF_DIAG {
            bail!(
                "RfDiag: bad ack 0x{:02X} (want 0x{ACK_RF_DIAG:02X}); raw={}",
                response[0],
                hexsp(response, 8)
            );
        }
        if response[1] == 0 {
            bail!("RfDiag: firmware does not know page {expected_page} (payload length 0)");
        }
        if response[1] as usize != PAGE_LEN || response.len() < 2 + PAGE_LEN {
            bail!(
                "RfDiag: payload length {} (expected {PAGE_LEN}); raw={}",
                response[1],
                hexsp(response, 8)
            );
        }
        let raw: [u8; PAGE_LEN] = response[2..2 + PAGE_LEN].try_into().unwrap();
        if raw[0] != PAGE_VERSION {
            bail!(
                "RfDiag: page version {} (this tool decodes v{PAGE_VERSION}); raw={}",
                raw[0],
                hexsp(&raw, PAGE_LEN)
            );
        }
        if raw[1] != expected_page {
            bail!("RfDiag: asked for page {expected_page}, got page {}", raw[1]);
        }
        Ok(Self { id: raw[1], raw })
    }
}

/// Human name of an `rf_state` value.
fn state_name(s: u8) -> &'static str {
    match s {
        0 => "idle",
        1 => "pairing",
        2 => "connected",
        _ => "?",
    }
}

/// Human name of a LEN-10 disposition code (RFD_L10_*).
fn disp_name(d: u8) -> &'static str {
    match d {
        0 => "none yet",
        1 => "accepted (known peer)",
        2 => "accepted (fresh pair)",
        3 => "rejected: bond tombstoned",
        4 => "rejected: bonded lockout (window closed, known-peer match failed)",
        5 => "rejected: invalid peer MAC (zero, all-FF or the dongle's own)",
        6 => "rejected: RSSI below floor",
        7 => "rejected: other gate",
        8 => "seen by EV10 reacquire branch",
        _ => "?",
    }
}

/// Human name of a persist-outcome code (RFD_PERSIST_*).
fn persist_name(r: u8) -> &'static str {
    match r {
        0 => "none yet",
        1 => "semantic-invalid record (not written)",
        2 => "bond_save failed",
        3 => "readback mismatch",
        4 => "tombstoned (not written)",
        5 => "stored record already matched (no write)",
        6 => "written and verified",
        _ => "?",
    }
}

/// Format a "last rc" byte, naming the 0xFF/0xFE sentinels.
fn rc(v: u8) -> String {
    match v {
        0xFF => "none yet".to_string(),
        0xFE => "refused (oversize)".to_string(),
        _ => format!("0x{v:02X}"),
    }
}

const SLOT_NAMES: [&str; 6] = [
    "pair-ack",
    "ev10-rekey",
    "boot-window",
    "connected-poll",
    "evt-delay-0",
    "evt-delay-1",
];

pub const PAGE2_NAMES: [&str; 15] = [
    "len10_seen",
    "len10_accept_known",
    "len10_accept_fresh",
    "len10_rejected",
    "len10_ev10_seen",
    "pair_ack_posted",
    "pair_ack_tx_ok",
    "pair_ack_tx_fail",
    "pair_ack_tx_done",
    "rx_restart_handled",
    "rx_restart_dropped_ev10",
    "arm_retry_scheduled",
    "pump_passes",
    "supervision_lapses",
    "ev10_entries",
];

pub const PAGE3_NAMES: [&str; 15] = [
    "ev10_giveups",
    "pair_prep_runs",
    "ev10_ack_tx",
    "ev10_ack_tx_fail",
    "connected_promotes",
    "fresh_relistens",
    "bootwin_closes",
    "confirm_armed",
    "confirm_to_waitrx",
    "confirm_to_waitrearm",
    "persist_attempts",
    "persist_ok",
    "ack_latency_ticks",
    "ack_tx_ticks",
    "last_ack_aa",
];

pub const PAGE4_NAMES: [&str; 3] = ["lle_irqs", "bb_irqs", "cb_calls"];

pub const PAGE1_NAMES: [&str; 9] = [
    "rx_arm_attempts",
    "rx_arm_fail",
    "rx_done",
    "rx_crcerr",
    "rx_timeout",
    "tx_start",
    "tx_fail",
    "tx_done",
    "shut_calls",
];

/// Page 5 words at [2 + 4 i]. `hal_now` is the 60 MHz SysTick clock (a gauge,
/// used as the idle-duty denominator); everything else is a wrapping counter.
pub const PAGE5_NAMES: [&str; 14] = [
    "wfe_count",
    "idle_tsys",
    "hb_irqs",
    "hal_now",
    "wake_tmr3",
    "wake_radio",
    "wake_both",
    "wake_tmr0",
    "wake_usb",
    "wake_other",
    "veto_work",
    "veto_pending",
    "veto_usb",
    "veto_state",
];

/// Page 6 words at [2 + 4 i]; the `sleep_control` and `*_ipr*` words are gauges.
/// [42] last-wake flags, [43] PB15 boot snapshot, [44..46] remote-wake arm
/// count (u16 on both sides: `usb_rw_arm_count`, delta modulo 2^16) and
/// [46..50] `wake_none` and [50..54] `stale_adc` (library temp-sample ADC residue cleared before sleeping) follow,
/// then [54..58] `hb_arm_deadline` and [58..62] `hb_arm_cap`: in the exact-deadline
/// heartbeat mode (page 5 flag 0x80), how many sleeps armed TMR3 to an app timer's
/// deadline versus to the cap; both stay 0 in the fixed-period mode. Page 5 [59] is
/// the idle level in its low nibble and, in that mode, a saturating count of
/// deadline-table entries retired more than 100 ms overdue (`hb_stale_drop`) in its high nibble:
/// any nonzero value is a finding.
///
/// `veto_entry` counts a post that landed after the previous masked idle
/// decision and that three scheduler passes then failed to dispatch (the
/// firmware arms it only from that decision point, so a latch left over from
/// a work veto never counts). It also ticks once when such a post is cancelled
/// before dispatch (a link-loss teardown sweeping the TMR0 ISR's post), so a
/// count of the order of the teardowns is benign; the scheduler-shape alarm
/// is a RATE comparable to `wfe_count` / `quiet_passes`.
pub const PAGE6_NAMES: [&str; 10] = [
    "sleep_control",
    "alien_ipr0",
    "alien_ipr1",
    "veto_ep0",
    "stale_tmr0",
    "veto_alien",
    "veto_entry",
    "quiet_passes",
    "last_wake_ipr0",
    "last_wake_ipr1",
];

/// The clock-gate oracle (page 6 `sleep_control`): the gated set is exactly
/// 0x4DF6 (TMR1/2, UART0-3, SPI0, PWMX, I2C, LCD) and the four live blocks -
/// TMR0 (RF pacer), TMR3 (heartbeat), USB and BLE - stay clocked. Byte 2 is
/// R8_SLP_WAKE_CTRL (0x20 at reset) and is not part of the check.
const CLK_GATE_MASK: u32 = 0x4DF6;
const CLK_LIVE_MASK: u32 = 0x0001 | 0x0008 | 0x1000 | 0x8000;

pub fn clock_gate_verdict(sleep_control: u32) -> String {
    let gated = sleep_control & 0xFFFF;
    let live_gated = gated & CLK_LIVE_MASK;
    if live_gated != 0 {
        format!("LIVE BLOCK GATED (mask 0x{live_gated:04X}: TMR0/TMR3/USB/BLE must stay clocked)")
    } else if gated == 0 {
        "gates off".to_string()
    } else if gated & !CLK_GATE_MASK != 0 {
        // A bit the firmware's static assert refuses: not a PM_CLK_GATE_MASK value.
        format!(
            "RESERVED BIT GATED (mask 0x{:04X}: outside the validated 0x{CLK_GATE_MASK:04X} set)",
            gated & !CLK_GATE_MASK
        )
    } else if gated == CLK_GATE_MASK {
        format!("gates=0x{gated:04X} (full validated set; TMR0/TMR3/USB/BLE clocked)")
    } else {
        // PM_CLK_GATE_MASK subsets are legitimate builds (bench bisection).
        format!("gates=0x{gated:04X} (permitted subset of 0x{CLK_GATE_MASK:04X}; TMR0/TMR3/USB/BLE clocked)")
    }
}

/// The counters that are 16 bits wide on the wire (widened to u32 in
/// `counters`); their per-sample delta wraps at 2^16, not 2^32.
const U16_COUNTERS: [&str; 2] = ["rw_arms", "camp_wd_rearms"];

/// The wrapping delta of a counter between two samples, in the counter's own
/// width: a u16 stepping 65535 -> 0 is +1, not 4_294_901_761.
pub fn counter_delta(name: &str, prev: u32, cur: u32) -> u32 {
    if U16_COUNTERS.contains(&name) {
        u32::from((cur as u16).wrapping_sub(prev as u16))
    } else {
        cur.wrapping_sub(prev)
    }
}

/// All the counters of a full sample, keyed by name, for rate printing (u16
/// counters widened; see `counter_delta`).
pub fn counters(pages: &[Page]) -> Vec<(&'static str, u32)> {
    let mut v = Vec::new();
    for p in pages {
        match p.id {
            1 => {
                for (i, n) in PAGE1_NAMES.iter().enumerate() {
                    v.push((*n, le32(&p.raw, 4 + 4 * i)));
                }
                v.push(("camp_wd_rearms", u32::from(le16(&p.raw, 60))));
            }
            2 => {
                for (i, n) in PAGE2_NAMES.iter().enumerate() {
                    v.push((*n, le32(&p.raw, 2 + 4 * i)));
                }
            }
            3 => {
                for (i, n) in PAGE3_NAMES.iter().enumerate() {
                    // The last three words are gauges (latency, duration, an
                    // address), not counters: a rate of them is meaningless.
                    if n.ends_with("_ticks") || *n == "last_ack_aa" {
                        continue;
                    }
                    v.push((*n, le32(&p.raw, 2 + 4 * i)));
                }
            }
            4 => {
                for (i, n) in PAGE4_NAMES.iter().enumerate() {
                    v.push((*n, le32(&p.raw, 2 + 4 * i)));
                }
            }
            5 => {
                for (i, n) in PAGE5_NAMES.iter().enumerate() {
                    if *n == "hal_now" {
                        continue; // a clock; the derived idle duty uses it
                    }
                    v.push((*n, le32(&p.raw, 2 + 4 * i)));
                }
            }
            6 => {
                for (i, n) in PAGE6_NAMES.iter().enumerate() {
                    if *n == "sleep_control" || n.contains("ipr") {
                        continue; // gauges / last-seen masks
                    }
                    v.push((*n, le32(&p.raw, 2 + 4 * i)));
                }
                v.push(("wake_none", le32(&p.raw, 46)));
                v.push(("rw_arms", u32::from(le16(&p.raw, 44))));
                v.push(("stale_adc", le32(&p.raw, 50)));
                v.push(("hb_arm_deadline", le32(&p.raw, 54)));
                v.push(("hb_arm_cap", le32(&p.raw, 58)));
            }
            _ => {}
        }
    }
    v
}

/// Render decoded pages as printable lines (every field, raw bytes last).
pub fn render(pages: &[Page]) -> Vec<String> {
    let mut out = Vec::new();
    for p in pages {
        let r = &p.raw;
        match p.id {
            0 => {
                let f0 = r[4];
                let f1 = r[5];
                out.push("rf runtime:".to_string());
                out.push(format!(
                    "  state           {} ({}) channel={} confirm_state={} boot_window_step={} pair_prep_idx={} data_ch_idx={}",
                    state_name(r[2]),
                    r[2],
                    r[3],
                    r[6],
                    r[7],
                    r[8],
                    r[9]
                ));
                out.push(format!(
                    "  flags           bond_valid={} persisted={} persist_pending={} ev10_active={} boot_window={} pair_window={} tombstone={} pair_is_fresh={}",
                    (f0 & 0x01) != 0,
                    (f0 & 0x02) != 0,
                    (f0 & 0x04) != 0,
                    (f0 & 0x08) != 0,
                    (f0 & 0x10) != 0,
                    (f0 & 0x20) != 0,
                    (f0 & 0x40) != 0,
                    (f0 & 0x80) != 0
                ));
                out.push(format!(
                    "                  quiesced={} burst_active={} first_supervision_armed={} rx_armed={}",
                    (f1 & 0x01) != 0,
                    (f1 & 0x02) != 0,
                    (f1 & 0x04) != 0,
                    (f1 & 0x08) != 0
                ));
                out.push(format!(
                    "  access addr     ram=0x{:08X} bond=0x{:08X} radio(last RX arm)=0x{:08X} pair-ack advertises 0x{:08X}",
                    le32(r, 12),
                    le32(r, 16),
                    le32(r, 20),
                    le32(r, 40)
                ));
                out.push(format!(
                    "  radio           last RX arm: channel={} rc={} timeout={} ({:.1} ms); last TX: channel={} rc={}; last shut rc={}; rx_armed={}",
                    r[24],
                    rc(r[25]),
                    le16(r, 28),
                    f64::from(le16(r, 28)) / 2000.0,
                    r[59],
                    rc(r[26]),
                    rc(r[27]),
                    r[60]
                ));
                out.push(format!(
                    "  peer            MAC {:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X} interval={} timeout={} rssi_byte={} ({})",
                    r[34], r[35], r[36], r[37], r[38], r[39],
                    le16(r, 44),
                    le16(r, 46),
                    r[10],
                    r[10] as i8
                ));
                out.push(format!(
                    "  clocks          hal_now=0x{:08X} last_conn_rx_tsys=0x{:08X} connected_rx_count={}",
                    le32(r, 30),
                    le32(r, 52),
                    le32(r, 48)
                ));
                out.push(format!(
                    "  last LEN-10     {} ({})",
                    disp_name(r[11]),
                    r[11]
                ));
                out.push(format!(
                    "  persist         {} ({}); last bond_save rc={}; last pair-ACK StartTx rc={}",
                    persist_name(r[56]),
                    r[56],
                    rc(r[57]),
                    rc(r[58])
                ));
                out.push(format!("  raw             {}", hexsp(r, PAGE_LEN)));
            }
            1 => {
                out.push("phy + executor:".to_string());
                let mut line = String::from("  phy            ");
                for (i, n) in PAGE1_NAMES.iter().enumerate() {
                    line.push_str(&format!(" {}={}", n, le32(r, 4 + 4 * i)));
                }
                out.push(line);
                out.push(format!(
                    "  camp watchdog   rearms={} (u16; ~5/s while a terminal camp is silent on CH59x, 0 while it hears)",
                    le16(r, 60)
                ));
                out.push(format!(
                    "  dispatch        pending=0x{:04X} delay_slot0=0x{:04X} delay_slot1=0x{:04X} degraded={}",
                    le16(r, 40),
                    le16(r, 42),
                    le16(r, 44),
                    le16(r, 46)
                ));
                let mask = r[2];
                let mut slots = String::new();
                for (i, name) in SLOT_NAMES.iter().enumerate() {
                    let armed = (mask >> i) & 1 == 1;
                    let periodic = r[3] != 0 && usize::from(r[3] - 1) == i;
                    let ms = le16(r, 48 + 2 * i) as i16;
                    slots.push_str(&format!(
                        " {}={}",
                        name,
                        if periodic {
                            "periodic".to_string()
                        } else if armed {
                            format!("{ms}ms")
                        } else {
                            "off".to_string()
                        }
                    ));
                }
                out.push(format!(
                    "  timers          active_mask=0x{:02X} periodic_owner={}{}",
                    mask,
                    if r[3] == 0 {
                        "none".to_string()
                    } else {
                        SLOT_NAMES
                            .get((r[3] - 1) as usize)
                            .map(|s| s.to_string())
                            .unwrap_or_else(|| format!("slot{}", r[3] - 1))
                    },
                    slots
                ));
                out.push(format!("  raw             {}", hexsp(r, PAGE_LEN)));
            }
            2 | 3 => {
                let names: &[&str] = if p.id == 2 { &PAGE2_NAMES } else { &PAGE3_NAMES };
                out.push(format!("protocol counters {}:", if p.id == 2 { "A" } else { "B" }));
                let mut line = String::from("                 ");
                for (i, n) in names.iter().enumerate() {
                    let v = le32(r, 2 + 4 * i);
                    if *n == "last_ack_aa" {
                        line.push_str(&format!(" {}=0x{:08X}", n, v));
                    } else if n.ends_with("_ticks") {
                        // Core-clock ticks; the page carries no clock, so the
                        // conversion assumes CH570's 100 MHz (CH592: 60 MHz).
                        line.push_str(&format!(" {}={} ({:.1} us @100MHz)", n, v, f64::from(v) / 100.0));
                    } else {
                        line.push_str(&format!(" {}={}", n, v));
                    }
                    if (i + 1) % 5 == 0 {
                        out.push(line);
                        line = String::from("                 ");
                    }
                }
                if !line.trim().is_empty() {
                    out.push(line);
                }
                out.push(format!("  raw             {}", hexsp(r, PAGE_LEN)));
            }
            4 => {
                let now = le32(r, 14);
                let age = |t: u32| -> String {
                    if t == 0 { "never".to_string() } else { format!("{:.3}s ago", f64::from(now.wrapping_sub(t)) / 1.0e8) }
                };
                out.push("radio internals:".to_string());
                out.push(format!(
                    "  irqs            lle_irqs={} bb_irqs={} cb_calls={} irq_bits=0x{:02X} (en BLEB={} BLEL={} TMR={}; hi BLEB={} BLEL={} TMR={}; regs_valid={})",
                    le32(r, 2), le32(r, 6), le32(r, 10), r[59],
                    r[59] & 1 != 0, r[59] & 2 != 0, r[59] & 4 != 0,
                    r[59] & 8 != 0, r[59] & 16 != 0, r[59] & 32 != 0, r[59] & 0x80 != 0
                ));
                out.push(format!(
                    "  systick         now=0x{:08X} last callback {} reacquire entry {} give-up {} (100 MHz; wraps every 42.9 s)",
                    now, age(le32(r, 18)), age(le32(r, 22)), age(le32(r, 26))
                ));
                let mode = le32(r, 30) & 3;
                out.push(format!(
                    "  LLE             ctrl=0x{:08X} (mode {}) status=0x{:08X} mask=0x{:08X} timeout=0x{:08X} BB[40]=0x{:08X}",
                    le32(r, 30),
                    match mode { 0 => "idle", 1 => "RX", 2 => "TX", _ => "?" },
                    le32(r, 34), le32(r, 38), le32(r, 42), le32(r, 46)
                ));
                out.push(format!(
                    "  lib             gStatus={} ({}) WaitRecvTime={} tuned_channel={} rx_white_channel={} burst_idx={} usb_suspend_episodes={}",
                    le32(r, 50),
                    match le32(r, 50) { 1 => "after SetRx (armed)", 2 => "after BB sync", 16 => "after StartTx", _ => "?" },
                    le16(r, 54), r[56], r[57], r[58], le16(r, 60)
                ));
                out.push(format!("  raw             {}", hexsp(r, PAGE_LEN)));
            }
            5 => {
                let f = r[58];
                let exact = f & 0x80 != 0;
                out.push("power:".to_string());
                out.push(format!(
                    "  idle            wfe_count={} idle_tsys={} hb_irqs={} hal_now=0x{:08X} level={} {}={} hb_stale_drop={}",
                    le32(r, 2), le32(r, 6), le32(r, 10), le32(r, 14), r[59] & 0x0F,
                    if exact { "deadline_cap_us" } else { "heartbeat_us" }, le16(r, 60), r[59] >> 4
                ));
                out.push(format!(
                    "  wakes           tmr3={} radio={} both={} tmr0={} usb={} other={}",
                    le32(r, 18), le32(r, 22), le32(r, 26), le32(r, 30), le32(r, 34), le32(r, 38)
                ));
                out.push(format!(
                    "  vetoes          work={} pending={} usb={} state={}",
                    le32(r, 42), le32(r, 46), le32(r, 50), le32(r, 54)
                ));
                out.push(format!(
                    "  flags           tmr0_counting={} tmr3_counting={} usb_configured={} usb_suspended={} idle_in_suspend={} debug_en={} remote_wake_armed={} exact_deadline={}",
                    f & 0x01 != 0, f & 0x02 != 0, f & 0x04 != 0, f & 0x08 != 0,
                    f & 0x10 != 0, f & 0x20 != 0, f & 0x40 != 0, exact
                ));
                out.push(format!("  raw             {}", hexsp(r, PAGE_LEN)));
            }
            6 => {
                let sc = le32(r, 2);
                let wf = r[42];
                let pb = r[43];
                out.push("power detail:".to_string());
                out.push(format!(
                    "  clock gates     sleep_control=0x{:08X} {}",
                    sc,
                    clock_gate_verdict(sc)
                ));
                out.push(format!(
                    "  vetoes          ep0={} entry={} alien={} (last alien IPR0=0x{:08X} IPR1=0x{:08X}) stale_tmr0={} stale_adc={} quiet_passes={} wake_none={}",
                    le32(r, 14), le32(r, 26), le32(r, 22), le32(r, 6), le32(r, 10),
                    le32(r, 18), le32(r, 50), le32(r, 30), le32(r, 46)
                ));
                out.push(format!(
                    "  heartbeat arms  deadline={} cap={} (exact-deadline mode only; 0/0 in the fixed-period mode)",
                    le32(r, 54), le32(r, 58)
                ));
                out.push(format!(
                    "  last wake       IPR0=0x{:08X} IPR1=0x{:08X} flags=0x{:02X} (tmr0_cyc={} tmr3_cyc={} usb_transfer={} usb_suspend={} usb_bus_rst={})",
                    le32(r, 34), le32(r, 38), wf,
                    wf & 0x01 != 0, wf & 0x02 != 0, wf & 0x04 != 0, wf & 0x08 != 0, wf & 0x10 != 0
                ));
                out.push(format!(
                    "  pb15 at boot    pu={} pd={} dir={} debug_en={}; remote-wake arms={}",
                    pb & 0x01 != 0, pb & 0x02 != 0, pb & 0x04 != 0, pb & 0x08 != 0, le16(r, 44)
                ));
                out.push(format!("  raw             {}", hexsp(r, PAGE_LEN)));
            }
            _ => out.push(format!("  page {} raw   {}", p.id, hexsp(r, PAGE_LEN))),
        }
    }
    out
}

/// Pages 0..=REQUIRED_PAGES-1 must exist; later pages are optional (an older
/// firmware answers an unknown page with payload length 0 and is skipped).
const REQUIRED_PAGES: u8 = 4;

/// Fetch all pages (unarmed exchanges only).
pub fn read_all(dev: &IapDevice) -> Result<Vec<Page>> {
    let mut pages = Vec::new();
    for id in 0..PAGE_COUNT {
        let response = op_rf_diag(dev, id)?
            .ok_or_else(|| anyhow::anyhow!("RfDiag page {id}: no response (timeout)"))?;
        if id >= REQUIRED_PAGES && response.len() >= 2 && response[0] == ACK_RF_DIAG && response[1] == 0 {
            continue; // firmware predates this page
        }
        pages.push(Page::decode(id, &response)?);
    }
    Ok(pages)
}

/// `show_rf_diag`: see the call sites; part of the diagnostics readout.
pub fn show_rf_diag(dev: &IapDevice) -> Result<Vec<Page>> {
    let pages = read_all(dev)?;
    for line in render(&pages) {
        println!("{line}");
    }
    Ok(pages)
}

/// Per-second rates of every counter between two samples `dt` seconds apart.
pub fn rates(prev: &[Page], cur: &[Page], dt: f64) -> Vec<String> {
    let a = counters(prev);
    let b = counters(cur);
    let mut out = Vec::new();
    let mut line = String::from("  rates/s        ");
    let mut n = 0;
    for ((name, va), (_, vb)) in a.iter().zip(b.iter()) {
        let d = counter_delta(name, *va, *vb);
        if d == 0 {
            continue;
        }
        line.push_str(&format!(" {}={:.1}", name, f64::from(d) / dt));
        n += 1;
        if n % 6 == 0 {
            out.push(line);
            line = String::from("                 ");
        }
    }
    if !line.trim().is_empty() {
        out.push(line);
    }
    if out.is_empty() {
        out.push("  rates/s         (no counter changed)".to_string());
    }
    if let Some(line) = power_derived(prev, cur, dt) {
        out.push(line);
    }
    out
}

/// Page 5 `idle_tsys` and `hal_now` are 32-bit counts of the 60 MHz SysTick,
/// so both wrap every 71.6 s. A per-sample delta is exact only while the sample
/// is shorter than that; a wrapped numerator cannot be recovered from host
/// time, so the duty is reported unavailable above this conservative bound
/// instead of silently low (90 % over 120 s would otherwise print ~30 %).
const TSYS_HZ: f64 = 60.0e6;
const TSYS_WRAP_S: f64 = 4_294_967_296.0 / TSYS_HZ;
const DUTY_MAX_DT_S: f64 = 60.0;

/// Values derived from page 5 deltas.
///
/// `idle_duty`: SysTick ticks spent in WFE over the SysTick ticks that
/// elapsed (host time stands in only if the firmware clock did not move),
/// exact for samples under `DUTY_MAX_DT_S`, `n/a` beyond.
///
/// `radio_wake_ratio` = `wake_both / (wake_radio + wake_both)`. The firmware
/// snapshots what is pending right after the WFE, before any ISR runs:
/// `wake_radio` counts exits where a radio IRQ was the ONLY pending source,
/// which proves the radio ended the WFE; `wake_both` counts exits where
/// another source (TMR3/TMR0/USB/alien) was pending too, and those the
/// counters cannot order - a radio IRQ that never woke the core and waited for
/// the next source looks the same as a radio wake with a heartbeat landing in
/// the read window. The ratio is therefore a co-pending frequency, not a
/// wake-failure rate. The R2a proof is the protocol, not the number: a
/// keyboard-absent negative control must read `wake_radio == wake_both == 0`,
/// then every reconnect/pair must advance `wake_radio`; a radio that never
/// ends the WFE shows as `wake_both` advancing with `wake_radio` flat.
fn power_derived(prev: &[Page], cur: &[Page], dt: f64) -> Option<String> {
    let a = prev.iter().find(|p| p.id == 5)?;
    let b = cur.iter().find(|p| p.id == 5)?;
    let d = |at: usize| le32(&b.raw, at).wrapping_sub(le32(&a.raw, at));
    let duty = if dt >= DUTY_MAX_DT_S {
        format!(
            "n/a (sample {dt:.1}s > {DUTY_MAX_DT_S:.0}s; idle_tsys wraps every {TSYS_WRAP_S:.1}s)"
        )
    } else {
        let elapsed = if d(14) != 0 { f64::from(d(14)) } else { dt * TSYS_HZ };
        format!("{:.1}%", 100.0 * f64::from(d(6)) / elapsed)
    };
    let radio = u64::from(d(22));
    let both = u64::from(d(26));
    let ratio = if radio + both == 0 {
        "n/a".to_string()
    } else {
        format!("{:.1}%", 100.0 * both as f64 / (radio + both) as f64)
    };
    Some(format!(
        "  derived         idle_duty={duty} radio_wake_ratio={ratio} (wake_both/(wake_radio+wake_both): radio-pending WFE exits with another source also pending, unordered; only wake_radio, radio pending alone, proves the radio ended the WFE - a non-waking radio reads wake_both up with wake_radio flat)"
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn page(id: u8, fill: impl Fn(&mut [u8; PAGE_LEN])) -> Vec<u8> {
        let mut raw = [0u8; PAGE_LEN];
        raw[0] = PAGE_VERSION;
        raw[1] = id;
        fill(&mut raw);
        let mut r = vec![ACK_RF_DIAG, PAGE_LEN as u8];
        r.extend_from_slice(&raw);
        r
    }

    #[test]
    fn decodes_runtime_page() {
        let r = page(0, |raw| {
            raw[2] = 1;
            raw[3] = 8;
            raw[4] = 0x03;
            raw[5] = 0x08;
            raw[12..16].copy_from_slice(&0x6AC4_86E6u32.to_le_bytes());
            raw[20..24].copy_from_slice(&0x6AC4_86E6u32.to_le_bytes());
            raw[25] = 0;
            raw[28..30].copy_from_slice(&60000u16.to_le_bytes());
            raw[60] = 1;
        });
        let p = Page::decode(0, &r).unwrap();
        let text = render(&[p]).join("\n");
        assert!(text.contains("state           pairing"), "{text}");
        assert!(text.contains("bond_valid=true persisted=true"), "{text}");
        assert!(text.contains("radio(last RX arm)=0x6AC486E6"), "{text}");
        assert!(text.contains("timeout=60000 (30.0 ms)"), "{text}");
        assert!(text.contains("rx_armed=1"), "{text}");
    }

    #[test]
    fn decodes_counters_and_rates() {
        let a = page(2, |raw| raw[2..6].copy_from_slice(&100u32.to_le_bytes()));
        let b = page(2, |raw| raw[2..6].copy_from_slice(&150u32.to_le_bytes()));
        let pa = Page::decode(2, &a).unwrap();
        let pb = Page::decode(2, &b).unwrap();
        assert_eq!(counters(std::slice::from_ref(&pa))[0], ("len10_seen", 100));
        let r = rates(&[pa], &[pb], 2.0).join("\n");
        assert!(r.contains("len10_seen=25.0"), "{r}");
    }

    #[test]
    fn timer_slots_render_remaining_ms() {
        let r = page(1, |raw| {
            raw[2] = 0x02;
            raw[48 + 2..48 + 4].copy_from_slice(&(1500i16).to_le_bytes());
        });
        let text = render(&[Page::decode(1, &r).unwrap()]).join("\n");
        assert!(text.contains("ev10-rekey=1500ms"), "{text}");
        assert!(text.contains("pair-ack=off"), "{text}");
        // The periodic grid owner has no one-shot remainder: shown as such.
        let r = page(1, |raw| {
            raw[2] = 0x08;
            raw[3] = 4;
        });
        let text = render(&[Page::decode(1, &r).unwrap()]).join("\n");
        assert!(text.contains("connected-poll=periodic"), "{text}");
        assert!(text.contains("periodic_owner=connected-poll"), "{text}");
    }

    #[test]
    fn decodes_radio_internals_page() {
        let r = page(4, |raw| {
            raw[2..6].copy_from_slice(&1000u32.to_le_bytes());
            raw[14..18].copy_from_slice(&200_000_000u32.to_le_bytes());
            raw[18..22].copy_from_slice(&100_000_000u32.to_le_bytes());
            raw[30..34].copy_from_slice(&1u32.to_le_bytes());
            raw[50..54].copy_from_slice(&1u32.to_le_bytes());
            raw[59] = 0x87;
        });
        let p = Page::decode(4, &r).unwrap();
        let text = render(std::slice::from_ref(&p)).join("\n");
        assert!(text.contains("lle_irqs=1000"), "{text}");
        assert!(text.contains("last callback 1.000s ago"), "{text}");
        assert!(text.contains("(mode RX)"), "{text}");
        assert!(text.contains("after SetRx (armed)"), "{text}");
        assert!(text.contains("regs_valid=true"), "{text}");
        assert_eq!(counters(&[p])[0], ("lle_irqs", 1000));
    }

    #[test]
    fn decodes_power_page_and_derives_duty() {
        let sample = |wfe: u32, idle: u32, now: u32, radio: u32, both: u32| {
            page(5, |raw| {
                raw[2..6].copy_from_slice(&wfe.to_le_bytes());
                raw[6..10].copy_from_slice(&idle.to_le_bytes());
                raw[14..18].copy_from_slice(&now.to_le_bytes());
                raw[22..26].copy_from_slice(&radio.to_le_bytes());
                raw[26..30].copy_from_slice(&both.to_le_bytes());
                raw[58] = 0x63; // TMR0+TMR3 counting, debug_en, remote wake armed
                raw[59] = 3;
                raw[60..62].copy_from_slice(&1000u16.to_le_bytes());
            })
        };
        let pa = Page::decode(5, &sample(1000, 0, 0, 10, 0)).unwrap();
        let pb = Page::decode(5, &sample(2000, 54_000_000, 60_000_000, 29, 1)).unwrap();
        let text = render(std::slice::from_ref(&pb)).join("\n");
        assert!(text.contains("level=3 heartbeat_us=1000 hb_stale_drop=0"), "{text}");
        assert!(text.contains("tmr3_counting=true"), "{text}");
        assert!(text.contains("remote_wake_armed=true"), "{text}");
        assert!(text.contains("wakes           tmr3=0 radio=29 both=1"), "{text}");
        assert!(counters(std::slice::from_ref(&pb)).iter().all(|(n, _)| *n != "hal_now"));
        let r = rates(std::slice::from_ref(&pa), std::slice::from_ref(&pb), 1.0).join("\n");
        assert!(r.contains("wfe_count=1000.0"), "{r}");
        assert!(r.contains("idle_duty=90.0%"), "{r}");
        assert!(r.contains("radio_wake_ratio=5.0%"), "{r}");
        // 90 % duty over 120 s: the u32 idle_tsys delta has wrapped once and
        // would read ~30 %; the tool must say so instead of printing it.
        let pc = Page::decode(5, &sample(121_000, 6_480_000_000u64 as u32, 0, 29, 1)).unwrap();
        let r = rates(&[pa], &[pc], 120.0).join("\n");
        assert!(r.contains("idle_duty=n/a (sample 120.0s > 60s; idle_tsys wraps every 71.6s)"), "{r}");
        // Under the bound the firmware clock is the denominator even when the
        // host interval disagrees (USB latency), and a stalled clock falls
        // back to host time rather than dividing by zero.
        let r = rates(std::slice::from_ref(&pb), &[Page::decode(5, &sample(3000, 84_000_000, 90_000_000, 29, 1)).unwrap()], 2.0).join("\n");
        assert!(r.contains("idle_duty=100.0%"), "{r}");
        let r = rates(std::slice::from_ref(&pb), &[Page::decode(5, &sample(3000, 114_000_000, 60_000_000, 29, 1)).unwrap()], 1.0).join("\n");
        assert!(r.contains("idle_duty=100.0%"), "{r}");
    }

    #[test]
    fn remote_wake_arm_count_deltas_as_u16() {
        let with = |arms: u16| {
            Page::decode(6, &page(6, |raw| raw[44..46].copy_from_slice(&arms.to_le_bytes()))).unwrap()
        };
        assert_eq!(counter_delta("rw_arms", 65535, 0), 1);
        assert_eq!(counter_delta("rw_arms", 3, 5), 2);
        assert_eq!(counter_delta("wfe_count", u32::MAX, 0), 1);
        let r = rates(&[with(65535)], &[with(0)], 1.0).join("\n");
        assert!(r.contains("rw_arms=1.0"), "{r}");
        assert!(!r.contains("4294901761"), "{r}");
    }

    #[test]
    fn power_detail_page_checks_the_clock_gate_oracle() {
        let with = |sc: u32| page(6, |raw| raw[2..6].copy_from_slice(&sc.to_le_bytes()));
        let text = render(&[Page::decode(6, &with(0x0020_4DF6)).unwrap()]).join("\n");
        assert!(text.contains("gates=0x4DF6 (full validated set"), "{text}");
        let text = render(&[Page::decode(6, &with(0x0020_4DFE)).unwrap()]).join("\n");
        assert!(text.contains("LIVE BLOCK GATED (mask 0x0008"), "{text}");
        let text = render(&[Page::decode(6, &with(0x0020_0000)).unwrap()]).join("\n");
        assert!(text.contains("gates off"), "{text}");
        let text = render(&[Page::decode(6, &with(0x0020_0006)).unwrap()]).join("\n");
        assert!(text.contains("gates=0x0006 (permitted subset of 0x4DF6"), "{text}");
        let text = render(&[Page::decode(6, &with(0x0020_0200)).unwrap()]).join("\n");
        assert!(text.contains("RESERVED BIT GATED (mask 0x0200"), "{text}");
        let p = Page::decode(
            6,
            &page(6, |raw| {
                raw[30..34].copy_from_slice(&7u32.to_le_bytes());
                raw[42] = 0x03;
                raw[43] = 0x08;
                raw[44..46].copy_from_slice(&3u16.to_le_bytes());
                raw[46..50].copy_from_slice(&2u32.to_le_bytes());
                raw[54..58].copy_from_slice(&11u32.to_le_bytes());
                raw[58..62].copy_from_slice(&900u32.to_le_bytes());
            }),
        )
        .unwrap();
        let text = render(std::slice::from_ref(&p)).join("\n");
        assert!(text.contains("heartbeat arms  deadline=11 cap=900"), "{text}");
        assert!(text.contains("tmr0_cyc=true tmr3_cyc=true usb_transfer=false"), "{text}");
        assert!(text.contains("pu=false pd=false dir=false debug_en=true; remote-wake arms=3"), "{text}");
        let c = counters(&[p]);
        assert!(c.contains(&("quiet_passes", 7)), "{c:?}");
        assert!(c.contains(&("wake_none", 2)), "{c:?}");
        assert!(c.contains(&("rw_arms", 3)), "{c:?}");
        assert!(c.contains(&("hb_arm_deadline", 11)), "{c:?}");
        assert!(c.contains(&("hb_arm_cap", 900)), "{c:?}");
        assert!(c.iter().all(|(n, _)| !n.contains("ipr") && *n != "sleep_control"), "{c:?}");
    }

    #[test]
    fn rejects_bad_framing() {
        assert!(Page::decode(0, &[ACK_RF_DIAG, 0]).is_err());
        let mut wrong_ver = page(0, |_| {});
        wrong_ver[2] = 9;
        assert!(Page::decode(0, &wrong_ver).is_err());
        let wrong_page = page(1, |_| {});
        assert!(Page::decode(0, &wrong_page).is_err());
        let mut bad_len = page(0, |_| {});
        bad_len[1] = 40;
        assert!(Page::decode(0, &bad_len).is_err());
    }
}
