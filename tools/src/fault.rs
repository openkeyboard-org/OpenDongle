// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! Decoder and concise display for the retained fault record (command 0x93).
//!
//! Two firmware producers emit the same 40-byte page:
//!
//! * CH570, page v5 — firmware/ch570/src/platform_ch570.c `dongle_fault_fill`
//! * CH592, page v1 — firmware/ch592/src/platform_ch592.c `dongle_fault_fill`
//!
//! | off | field |
//! |-----|-------|
//! | 0 | chip family |
//! | 1 | page version |
//! | 2 | record valid |
//! | 3 | record version |
//! | 4..6 | kind, action, flags |
//! | 7 | live `R8_GLOB_RESET_KEEP` |
//! | 8..24 | event / reset-request / repeat / boot counts (LE32) |
//! | 24..36 | mcause, mepc, mtval (LE32) |
//! | 36 | record `last_reset_status` (written at boot, overwritten at fault time) |
//! | 37 | startup phase marker (stamped by the reset handler) |
//! | 38 | reset status captured pristine at startup, before any init |
//! | 39 | record `reset_keep` snapshot |
//!
//! Bytes 0..3, 7, 37 and 38 are populated on every read. The producer copies
//! the remaining record-derived bytes only when byte 2 (record valid) is set
//! and otherwise leaves them as zero fill, so for an invalid record this
//! decoder reports those fields as unavailable instead of naming the zeros a
//! software reset or a cleared keeper. An unrecognised (family, page version)
//! pair gets a warning and nothing decoded. The raw page is always printed.

use anyhow::{bail, Result};

use crate::iap::{hexsp, op_fault, IapDevice};

const ACK_FAULT: u8 = 0x93;
const FAULT_LEN: usize = 40;

const KEEP_ARMED: u8 = 0x5C;
const KEEP_CONSUMED: u8 = 0xA3;

const FLAG_REBUILT: u8 = 0x01;
const FLAG_RESET_MISMATCH: u8 = 0x02;

/// (family, page version, name) triples whose layout is the one documented
/// above. Both chips define identical kind/action/flag values.
const KNOWN_LAYOUTS: &[(u8, u8, &str)] = &[(0x70, 5, "CH570"), (0x92, 1, "CH592")];

fn known_chip(family: u8, page_version: u8) -> Option<&'static str> {
    KNOWN_LAYOUTS
        .iter()
        .find(|&&(f, v, _)| f == family && v == page_version)
        .map(|&(_, _, name)| name)
}

fn known_layouts_list() -> String {
    KNOWN_LAYOUTS
        .iter()
        .map(|&(f, v, name)| format!("{name} 0x{f:02X} v{v}"))
        .collect::<Vec<_>>()
        .join(", ")
}

#[derive(Debug, PartialEq, Eq)]
struct FaultStatus {
    family: u8,
    page_version: u8,
    valid: bool,
    record_version: u8,
    kind: u8,
    action: u8,
    flags: u8,
    keeper_live: u8,
    event_count: u32,
    reset_request_count: u32,
    repeat_count: u32,
    boot_count: u32,
    mcause: u32,
    mepc: u32,
    mtval: u32,
    /// Record byte: the reset status the boot path (or the fault handler) stored.
    record_reset_status: u8,
    /// Startup phase marker stamped by the reset handler (CH570: 0xC0..0xC5).
    startup_marker: u8,
    /// Reset status read pristine at startup, before clock/peripheral init.
    startup_reset_status: u8,
    /// Record's own snapshot of the reset keeper.
    reset_keep: u8,
    raw: [u8; FAULT_LEN],
}

fn reset_name(status: u8) -> &'static str {
    match status & 0x07 {
        0 => "software",
        1 => "power-on",
        2 => "watchdog",
        3 => "external",
        5 => "wake",
        _ => "unknown",
    }
}

fn keeper_name(keep: u8) -> &'static str {
    match keep {
        KEEP_ARMED => "armed",
        KEEP_CONSUMED => "consumed",
        0 => "cleared",
        _ => "other",
    }
}

fn flag_names(flags: u8) -> String {
    if flags == 0 {
        return "none".to_string();
    }
    let mut names = Vec::new();
    if flags & FLAG_REBUILT != 0 {
        names.push("rebuilt".to_string());
    }
    if flags & FLAG_RESET_MISMATCH != 0 {
        names.push("reset-mismatch".to_string());
    }
    let rest = flags & !(FLAG_REBUILT | FLAG_RESET_MISMATCH);
    if rest != 0 {
        names.push(format!("unknown 0x{rest:02X}"));
    }
    names.join(", ")
}

impl FaultStatus {
    /// Split the 40-byte page into fields. Succeeds for any well-framed
    /// response; whether the (family, page version) pair is one this tool
    /// knows how to interpret is answered by [`chip`](Self::chip), so an
    /// unexpected page still reaches the display and its raw bytes are shown.
    fn decode(response: &[u8]) -> Result<Self> {
        if response.len() < 2 + FAULT_LEN {
            bail!(
                "FaultRead: short response ({} bytes, expected at least {})",
                response.len(),
                2 + FAULT_LEN
            );
        }
        if response[0] != ACK_FAULT {
            bail!(
                "FaultRead: bad ack 0x{:02X} (want 0x{ACK_FAULT:02X}); raw={}",
                response[0],
                hexsp(response, 8)
            );
        }
        if response[1] as usize != FAULT_LEN {
            bail!(
                "FaultRead: payload length {} (expected {FAULT_LEN})",
                response[1]
            );
        }
        let payload = &response[2..2 + FAULT_LEN];
        let le32 = |at: usize| u32::from_le_bytes(payload[at..at + 4].try_into().unwrap());
        Ok(Self {
            family: payload[0],
            page_version: payload[1],
            valid: payload[2] != 0,
            record_version: payload[3],
            kind: payload[4],
            action: payload[5],
            flags: payload[6],
            keeper_live: payload[7],
            event_count: le32(8),
            reset_request_count: le32(12),
            repeat_count: le32(16),
            boot_count: le32(20),
            mcause: le32(24),
            mepc: le32(28),
            mtval: le32(32),
            record_reset_status: payload[36],
            startup_marker: payload[37],
            startup_reset_status: payload[38],
            reset_keep: payload[39],
            raw: payload.try_into().unwrap(),
        })
    }

    /// Chip name when the (family, page version) pair is a layout this tool
    /// understands; `None` means the field bytes must not be interpreted.
    fn chip(&self) -> Option<&'static str> {
        known_chip(self.family, self.page_version)
    }

    fn startup_reset_name(&self) -> &'static str {
        reset_name(self.startup_reset_status)
    }

    fn record_reset_name(&self) -> &'static str {
        reset_name(self.record_reset_status)
    }

    fn fault_name(&self) -> &'static str {
        match self.kind {
            0x00 => "none",
            0xDE => "hard fault",
            0xDF => "NMI",
            _ => "unknown",
        }
    }

    fn action_name(&self) -> &'static str {
        match self.action {
            0 => "none",
            1 => "reset requested",
            2 => "recovered",
            3 => "repeat fail-stop",
            4 => "guard only",
            5 => "reset mismatch",
            _ => "unknown",
        }
    }
}

fn line(label: &str, value: impl std::fmt::Display) -> String {
    format!("  {label:<16}{value}")
}

/// The fields that exist only inside a valid record; for an invalid one the
/// producer leaves their bytes zero and they are shown as unavailable.
const RECORD_ONLY_LABELS: &[&str] = &[
    "record reset",
    "record keeper",
    "last fault",
    "action",
    "flags",
    "counters",
    "mcause",
    "mepc",
    "mtval",
];

/// Render the health block. Split from printing so tests can pin the exact
/// wording: which fields appear, which say unavailable, and that the raw page
/// is always the last line.
fn render_fault(fault: &FaultStatus) -> Vec<String> {
    let mut lines = vec!["health:".to_string()];
    let raw_line = line("raw", hexsp(&fault.raw, FAULT_LEN));

    let Some(chip) = fault.chip() else {
        lines.push(line(
            "WARNING",
            format!(
                "unrecognized fault page family=0x{:02X} version={}; known layouts: {}. \
                 Fields not decoded.",
                fault.family,
                fault.page_version,
                known_layouts_list()
            ),
        ));
        lines.push(raw_line);
        return lines;
    };

    // Always populated by the producer, valid record or not.
    lines.push(line(
        "page",
        format!(
            "v{} family=0x{:02X} ({chip}) valid={} record-version={}",
            fault.page_version,
            fault.family,
            if fault.valid { "yes" } else { "no" },
            if fault.valid {
                fault.record_version.to_string()
            } else {
                "unavailable".to_string()
            }
        ),
    ));
    lines.push(line(
        "startup reset",
        format!(
            "{} (0x{:02X})",
            fault.startup_reset_name(),
            fault.startup_reset_status
        ),
    ));
    lines.push(line(
        "startup marker",
        format!("0x{:02X}", fault.startup_marker),
    ));
    lines.push(line(
        "reset keeper",
        format!(
            "live=0x{:02X} ({})",
            fault.keeper_live,
            keeper_name(fault.keeper_live)
        ),
    ));

    if fault.valid {
        lines.push(line(
            "record reset",
            format!(
                "{} (0x{:02X})",
                fault.record_reset_name(),
                fault.record_reset_status
            ),
        ));
        lines.push(line(
            "record keeper",
            format!(
                "0x{:02X} ({})",
                fault.reset_keep,
                keeper_name(fault.reset_keep)
            ),
        ));
        lines.push(line(
            "last fault",
            format!("{} (kind 0x{:02X})", fault.fault_name(), fault.kind),
        ));
        lines.push(line(
            "action",
            format!("{} ({})", fault.action_name(), fault.action),
        ));
        lines.push(line(
            "flags",
            format!("0x{:02X} ({})", fault.flags, flag_names(fault.flags)),
        ));
        lines.push(line(
            "counters",
            format!(
                "events={} reset-requests={} repeats={} boots={}",
                fault.event_count, fault.reset_request_count, fault.repeat_count, fault.boot_count
            ),
        ));
        lines.push(line("mcause", format!("0x{:08X}", fault.mcause)));
        lines.push(line("mepc", format!("0x{:08X}", fault.mepc)));
        lines.push(line("mtval", format!("0x{:08X}", fault.mtval)));
    } else {
        for label in RECORD_ONLY_LABELS {
            lines.push(line(label, "unavailable"));
        }
        lines.push(line(
            "note",
            "record invalid: the firmware fills record fields only for a valid \
             record, so the zero bytes in the raw page are fill, not values",
        ));
    }
    lines.push(raw_line);
    lines
}

fn print_fault(fault: &FaultStatus) {
    for text in render_fault(fault) {
        println!("{text}");
    }
}

/// Read and print the fault page. Command 0x93 is deliberately unarmed on the
/// firmware side, so this neither arms nor disarms a session and can be
/// issued on a unit whose RF link must not be disturbed any more than one EP6
/// exchange does.
pub fn show_fault_info(dev: &IapDevice) -> Result<()> {
    let response =
        op_fault(dev)?.ok_or_else(|| anyhow::anyhow!("FaultRead: no response (timeout)"))?;
    let fault = FaultStatus::decode(&response)?;
    print_fault(&fault);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A clean CH570 page: valid record, no fault, three boots, armed keeper.
    fn response() -> Vec<u8> {
        let mut response = vec![ACK_FAULT, FAULT_LEN as u8];
        let mut payload = [0u8; FAULT_LEN];
        payload[0] = 0x70;
        payload[1] = 5;
        payload[2] = 1;
        payload[3] = 1;
        payload[7] = KEEP_ARMED;
        payload[20..24].copy_from_slice(&3u32.to_le_bytes());
        payload[36] = 3;
        payload[37] = 0xC5;
        payload[38] = 1;
        payload[39] = KEEP_ARMED;
        response.extend_from_slice(&payload);
        response
    }

    /// What `dongle_fault_fill` emits for an invalid record: only bytes 0..3,
    /// 7, 37 and 38 carry values; everything else is the zero fill.
    fn invalid_response() -> Vec<u8> {
        let mut response = vec![ACK_FAULT, FAULT_LEN as u8];
        let mut payload = [0u8; FAULT_LEN];
        payload[0] = 0x70;
        payload[1] = 5;
        payload[2] = 0;
        payload[7] = KEEP_ARMED;
        payload[37] = 0xC5;
        payload[38] = 1;
        response.extend_from_slice(&payload);
        response
    }

    fn rendered(response: &[u8]) -> String {
        let lines = render_fault(&FaultStatus::decode(response).unwrap());
        lines.join("\n")
    }

    #[test]
    fn decodes_healthy_record() {
        let fault = FaultStatus::decode(&response()).unwrap();
        assert!(fault.valid);
        assert_eq!(fault.family, 0x70);
        assert_eq!(fault.page_version, 5);
        assert_eq!(fault.chip(), Some("CH570"));
        assert_eq!(fault.record_version, 1);
        assert_eq!(fault.boot_count, 3);
        assert_eq!(fault.startup_reset_name(), "power-on");
        assert_eq!(fault.record_reset_name(), "external");
        assert_eq!(fault.startup_marker, 0xC5);
        assert_eq!(keeper_name(fault.keeper_live), "armed");
        assert_eq!(fault.reset_keep, KEEP_ARMED);
        assert_eq!(fault.fault_name(), "none");
        assert_eq!(&fault.raw[..], &response()[2..]);
    }

    #[test]
    fn decodes_fault_details() {
        let mut response = response();
        response[2 + 4] = 0xDE;
        response[2 + 5] = 2;
        response[2 + 7] = KEEP_CONSUMED;
        response[2 + 8..2 + 12].copy_from_slice(&1u32.to_le_bytes());
        response[2 + 12..2 + 16].copy_from_slice(&1u32.to_le_bytes());
        response[2 + 24..2 + 28].copy_from_slice(&2u32.to_le_bytes());
        response[2 + 28..2 + 32].copy_from_slice(&0x1234u32.to_le_bytes());
        response[2 + 36] = 0; /* fault handler stores the status it saw: software */
        let fault = FaultStatus::decode(&response).unwrap();
        assert_eq!(fault.fault_name(), "hard fault");
        assert_eq!(fault.action_name(), "recovered");
        assert_eq!(fault.event_count, 1);
        assert_eq!(fault.reset_request_count, 1);
        assert_eq!(fault.mcause, 2);
        assert_eq!(fault.mepc, 0x1234);
        assert_eq!(keeper_name(fault.keeper_live), "consumed");
        assert_eq!(fault.record_reset_name(), "software");
        assert_eq!(fault.startup_reset_name(), "power-on");
    }

    #[test]
    fn reset_names_mask_to_three_bits() {
        assert_eq!(reset_name(0x00), "software");
        assert_eq!(reset_name(0x01), "power-on");
        assert_eq!(reset_name(0x02), "watchdog");
        assert_eq!(reset_name(0x03), "external");
        assert_eq!(reset_name(0x05), "wake");
        assert_eq!(reset_name(0xF1), "power-on");
        assert_eq!(reset_name(0x04), "unknown");
    }

    #[test]
    fn rejects_malformed_fault_response() {
        assert!(FaultStatus::decode(&[ACK_FAULT, FAULT_LEN as u8]).is_err());
        let mut wrong_length = response();
        wrong_length[1] = 39;
        assert!(FaultStatus::decode(&wrong_length).is_err());
        let mut wrong_ack = response();
        wrong_ack[0] = 0x91;
        assert!(FaultStatus::decode(&wrong_ack).is_err());
    }

    /// A valid record prints every field, including the ones that are zero
    /// on a clean unit: action, flags, all four counters and the trap CSRs.
    /// Nothing is "unavailable" and the raw page closes the block.
    #[test]
    fn valid_page_renders_every_field() {
        let text = rendered(&response());
        assert!(text.contains("valid=yes record-version=1"), "{text}");
        assert!(text.contains("family=0x70 (CH570)"), "{text}");
        assert!(text.contains("startup reset   power-on (0x01)"), "{text}");
        assert!(text.contains("startup marker  0xC5"), "{text}");
        assert!(text.contains("reset keeper    live=0x5C (armed)"), "{text}");
        assert!(text.contains("record reset    external (0x03)"), "{text}");
        assert!(text.contains("record keeper   0x5C (armed)"), "{text}");
        assert!(text.contains("last fault      none (kind 0x00)"), "{text}");
        assert!(text.contains("action          none (0)"), "{text}");
        assert!(text.contains("flags           0x00 (none)"), "{text}");
        assert!(
            text.contains("counters        events=0 reset-requests=0 repeats=0 boots=3"),
            "{text}"
        );
        assert!(text.contains("mcause          0x00000000"), "{text}");
        assert!(text.contains("mepc            0x00000000"), "{text}");
        assert!(text.contains("mtval           0x00000000"), "{text}");
        assert!(!text.contains("unavailable"), "{text}");
        assert!(!text.contains("WARNING"), "{text}");
        let last = text.lines().last().unwrap();
        assert!(last.starts_with("  raw             70 05 01 01"), "{last}");
    }

    /// For an invalid record the producer never copies the record fields, so
    /// their zero bytes must read as unavailable, not as a software reset or
    /// a cleared keeper. The always-populated bytes still print, as does raw.
    #[test]
    fn invalid_page_renders_record_fields_as_unavailable() {
        let text = rendered(&invalid_response());
        assert!(
            text.contains("valid=no record-version=unavailable"),
            "{text}"
        );
        assert!(text.contains("startup reset   power-on (0x01)"), "{text}");
        assert!(text.contains("startup marker  0xC5"), "{text}");
        assert!(text.contains("reset keeper    live=0x5C (armed)"), "{text}");
        for label in RECORD_ONLY_LABELS {
            assert!(
                text.contains(&line(label, "unavailable")),
                "{label}: {text}"
            );
        }
        assert!(!text.contains("software"), "{text}");
        assert!(!text.contains("cleared"), "{text}");
        assert!(!text.contains("events="), "{text}");
        assert!(text.contains("  raw             70 05 00 00"), "{text}");
    }

    /// Distinct sentinel values in the fields that used to be printed only
    /// conditionally, so a regression to "hide when zero" or a byte-offset
    /// slip shows up as a missing or wrong number.
    #[test]
    fn sentinel_values_reach_the_output() {
        let mut response = response();
        response[2 + 4] = 0xDF; // NMI
        response[2 + 5] = 3; // repeat fail-stop
        response[2 + 6] = FLAG_REBUILT | FLAG_RESET_MISMATCH;
        response[2 + 8..2 + 12].copy_from_slice(&0x0102_0304u32.to_le_bytes());
        response[2 + 12..2 + 16].copy_from_slice(&5u32.to_le_bytes());
        response[2 + 16..2 + 20].copy_from_slice(&7u32.to_le_bytes());
        response[2 + 20..2 + 24].copy_from_slice(&9u32.to_le_bytes());
        response[2 + 24..2 + 28].copy_from_slice(&0x0000_000Bu32.to_le_bytes());
        response[2 + 28..2 + 32].copy_from_slice(&0x0000_2ABCu32.to_le_bytes());
        response[2 + 32..2 + 36].copy_from_slice(&0xDEAD_BEEFu32.to_le_bytes());
        let text = rendered(&response);
        assert!(text.contains("last fault      NMI (kind 0xDF)"), "{text}");
        assert!(
            text.contains("action          repeat fail-stop (3)"),
            "{text}"
        );
        assert!(
            text.contains("flags           0x03 (rebuilt, reset-mismatch)"),
            "{text}"
        );
        assert!(
            text.contains("counters        events=16909060 reset-requests=5 repeats=7 boots=9"),
            "{text}"
        );
        assert!(text.contains("mcause          0x0000000B"), "{text}");
        assert!(text.contains("mepc            0x00002ABC"), "{text}");
        assert!(text.contains("mtval           0xDEADBEEF"), "{text}");
    }

    #[test]
    fn flag_names_cover_known_and_unknown_bits() {
        assert_eq!(flag_names(0), "none");
        assert_eq!(flag_names(FLAG_REBUILT), "rebuilt");
        assert_eq!(flag_names(FLAG_RESET_MISMATCH), "reset-mismatch");
        assert_eq!(flag_names(0x03), "rebuilt, reset-mismatch");
        assert_eq!(flag_names(0x81), "rebuilt, unknown 0x80");
    }

    /// The CH592 producer emits the same 40 bytes under family 0x92, page v1.
    #[test]
    fn ch592_page_v1_shares_the_layout() {
        let mut response = response();
        response[2] = 0x92;
        response[2 + 1] = 1;
        let fault = FaultStatus::decode(&response).unwrap();
        assert_eq!(fault.chip(), Some("CH592"));
        let text = render_fault(&fault).join("\n");
        assert!(text.contains("v1 family=0x92 (CH592) valid=yes"), "{text}");
        assert!(text.contains("counters        events=0"), "{text}");
        assert!(!text.contains("WARNING"), "{text}");
    }

    /// An unknown (family, version) pair still decodes (so the bytes reach
    /// the operator) but prints only a warning and the raw page.
    #[test]
    fn unknown_layout_warns_and_shows_raw_only() {
        for (family, version) in [(0x71u8, 5u8), (0x70, 4), (0x70, 6), (0x92, 5), (0x70, 0)] {
            let mut response = response();
            response[2] = family;
            response[2 + 1] = version;
            let fault = FaultStatus::decode(&response).unwrap();
            assert_eq!(fault.chip(), None, "{family:02X}/{version}");
            let lines = render_fault(&fault);
            assert_eq!(lines.len(), 3, "{lines:?}");
            assert_eq!(lines[0], "health:");
            assert!(lines[1].contains("WARNING"), "{}", lines[1]);
            assert!(
                lines[1].contains(&format!(
                    "unrecognized fault page family=0x{family:02X} version={version}"
                )),
                "{}",
                lines[1]
            );
            assert!(lines[1].contains("CH570 0x70 v5"), "{}", lines[1]);
            assert!(lines[1].contains("CH592 0x92 v1"), "{}", lines[1]);
            assert!(
                lines[2].starts_with(&format!(
                    "  raw             {family:02x} {version:02x} 01 01"
                )),
                "{}",
                lines[2]
            );
            let text = lines.join("\n");
            assert!(!text.contains("counters"), "{text}");
            assert!(!text.contains("startup"), "{text}");
        }
    }
}
