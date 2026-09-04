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
pub const PAGE_COUNT: u8 = 5;
const PAGE_VERSION: u8 = 1;

fn le16(p: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([p[at], p[at + 1]])
}
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

fn state_name(s: u8) -> &'static str {
    match s {
        0 => "idle",
        1 => "pairing",
        2 => "connected",
        _ => "?",
    }
}

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

/// All the u32 counters of a full sample, keyed by name, for rate printing.
pub fn counters(pages: &[Page]) -> Vec<(&'static str, u32)> {
    let mut v = Vec::new();
    for p in pages {
        match p.id {
            1 => {
                for (i, n) in PAGE1_NAMES.iter().enumerate() {
                    v.push((*n, le32(&p.raw, 4 + 4 * i)));
                }
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
            _ => {}
        }
    }
    v
}

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
                if line.trim().len() > 0 {
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
        let d = vb.wrapping_sub(*va);
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
    if line.trim().len() > 0 {
        out.push(line);
    }
    if out.is_empty() {
        out.push("  rates/s         (no counter changed)".to_string());
    }
    out
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
        assert_eq!(counters(&[pa.clone()])[0], ("len10_seen", 100));
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
        let text = render(&[p.clone()]).join("\n");
        assert!(text.contains("lle_irqs=1000"), "{text}");
        assert!(text.contains("last callback 1.000s ago"), "{text}");
        assert!(text.contains("(mode RX)"), "{text}");
        assert!(text.contains("after SetRx (armed)"), "{text}");
        assert!(text.contains("regs_valid=true"), "{text}");
        assert_eq!(counters(&[p])[0], ("lle_irqs", 1000));
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
