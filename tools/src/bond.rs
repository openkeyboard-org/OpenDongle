// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! Read-only decoding and display for the firmware's persistent bond record.

use anyhow::{bail, Result};

use crate::iap::{
    check, hexsp, op_arm, op_bond_read, op_disarm, IapDevice, ACK_GETDEVINFO, ACK_OK,
};

const BOND_MAGIC: u32 = 0x444E_4F42;
const BOND_VERSION: u8 = 1;
const BOND_RECORD_LEN: usize = 32;
const ACK_BOND_READ: u8 = 0x88;

#[derive(Debug, PartialEq, Eq)]
struct BondRecord {
    magic: u32,
    version: u8,
    flags: u8,
    conn_interval: u16,
    session_aa: u32,
    conn_timeout: u16,
    reserved0: u16,
    /// N09 dongle-identity override; all-zero means "use the factory MAC".
    dongle_mac: [u8; 6],
    peer_mac: [u8; 6],
    checksum: u32,
    raw: [u8; BOND_RECORD_LEN],
}

enum BondRead {
    Valid(BondRecord),
    /// The firmware said the record is not usable. The fields are the same
    /// byte-for-byte split as for a valid record, but nothing about them has
    /// been checked, so they are shown as a best-effort reading only.
    Invalid(BondRecord),
}

impl BondRecord {
    /// Split the 32 bytes into fields with no validation at all.
    fn parse(raw: &[u8; BOND_RECORD_LEN]) -> Self {
        Self {
            magic: u32::from_le_bytes(raw[0..4].try_into().unwrap()),
            version: raw[4],
            flags: raw[5],
            conn_interval: u16::from_le_bytes(raw[6..8].try_into().unwrap()),
            session_aa: u32::from_le_bytes(raw[8..12].try_into().unwrap()),
            conn_timeout: u16::from_le_bytes(raw[12..14].try_into().unwrap()),
            reserved0: u16::from_le_bytes(raw[14..16].try_into().unwrap()),
            dongle_mac: raw[16..22].try_into().unwrap(),
            peer_mac: raw[22..28].try_into().unwrap(),
            checksum: u32::from_le_bytes(raw[28..32].try_into().unwrap()),
            raw: *raw,
        }
    }

    /// The firmware's checksum: a plain byte sum over the first 28 bytes.
    fn computed_checksum(&self) -> u32 {
        self.raw[..28].iter().map(|&byte| u32::from(byte)).sum()
    }

    /// Cross-check a record the firmware marked valid. A failure here means
    /// the host and firmware disagree about the format, which is an error,
    /// not information.
    fn validate(&self) -> Result<()> {
        if self.magic != BOND_MAGIC {
            bail!(
                "BondRead: firmware marked a record valid with bad magic 0x{:08X}",
                self.magic
            );
        }
        if self.version != BOND_VERSION {
            bail!(
                "BondRead: firmware marked unsupported bond version {} valid",
                self.version
            );
        }
        if self.session_aa == 0 {
            bail!("BondRead: firmware marked a zero session address valid");
        }
        let expected_checksum = self.computed_checksum();
        if self.checksum != expected_checksum {
            bail!(
                "BondRead: firmware marked a record valid with checksum 0x{:08X} (expected 0x{expected_checksum:08X})",
                self.checksum
            );
        }
        Ok(())
    }

    fn decode(raw: &[u8; BOND_RECORD_LEN]) -> Result<Self> {
        let record = Self::parse(raw);
        record.validate()?;
        Ok(record)
    }
}

fn decode_response(response: &[u8]) -> Result<BondRead> {
    if response.len() < 3 + BOND_RECORD_LEN {
        bail!(
            "BondRead: short response ({} bytes, expected at least {})",
            response.len(),
            3 + BOND_RECORD_LEN
        );
    }
    if response[0] != ACK_BOND_READ {
        bail!(
            "BondRead: bad ack 0x{:02X} (want 0x{ACK_BOND_READ:02X}); raw={}",
            response[0],
            hexsp(response, 8)
        );
    }
    if response[1] as usize != BOND_RECORD_LEN {
        bail!(
            "BondRead: record length {} (expected {BOND_RECORD_LEN}); raw={}",
            response[1],
            hexsp(response, 8)
        );
    }

    let raw: [u8; BOND_RECORD_LEN] = response[3..3 + BOND_RECORD_LEN].try_into().unwrap();
    match response[2] {
        0 => Ok(BondRead::Valid(BondRecord::decode(&raw)?)),
        1 => Ok(BondRead::Invalid(BondRecord::parse(&raw))),
        status => bail!("BondRead: unknown validity status 0x{status:02X}"),
    }
}

fn format_mac(mac: &[u8; 6]) -> String {
    mac.iter()
        .map(|byte| format!("{byte:02X}"))
        .collect::<Vec<_>>()
        .join(":")
}

fn line(label: &str, value: impl std::fmt::Display) -> String {
    format!("  {label:<16}{value}")
}

fn ticks_line(label: &str, ticks: u16) -> String {
    line(
        label,
        format!("{ticks} ticks ({:.3} ms)", f64::from(ticks) / 32.0),
    )
}

/// Render the bond block. Split from printing so tests can pin which fields
/// appear for a valid record, what a best-effort reading of an invalid one
/// looks like, and that the raw bytes always close the block.
fn render_bond(read: &BondRead) -> Vec<String> {
    let mut lines = vec!["bond:".to_string()];
    match read {
        BondRead::Invalid(record) => {
            lines.push(line(
                "valid",
                "no (not present, invalid, or NV read failed)",
            ));
            if record.raw.iter().all(|&byte| byte == 0) {
                // handle_bond_read zero-initialises the record and bond_load
                // returns early without filling it on an NV read error
                // (firmware/common/src/iap.c), so a failed read comes back as
                // all zeros. The converse does not hold: a record that really
                // is all zeros, or was corrupted to zeros, looks identical.
                lines.push(line(
                    "note",
                    "all-zero record: may indicate a failed NV read (bond_load leaves the \
                     zero-initialised record untouched on an NV error); a genuinely all-zero \
                     or corrupt record is indistinguishable",
                ));
            }
            lines.push(line(
                "best-effort",
                "fields below are split from an invalid record and are not trusted",
            ));
            lines.push(line(
                "magic",
                format!("0x{:08X} (expected 0x{BOND_MAGIC:08X})", record.magic),
            ));
            lines.push(line(
                "format",
                format!("{} (expected {BOND_VERSION})", record.version),
            ));
            lines.push(line("flags", format!("0x{:02X}", record.flags)));
            lines.push(line("session AA", format!("0x{:08X}", record.session_aa)));
            lines.push(ticks_line("interval", record.conn_interval));
            lines.push(ticks_line("timeout", record.conn_timeout));
            lines.push(line("keyboard MAC", format_mac(&record.peer_mac)));
            lines.push(line("dongle MAC", format_mac(&record.dongle_mac)));
            lines.push(line("reserved", format!("0x{:04X}", record.reserved0)));
            let computed = record.computed_checksum();
            lines.push(line(
                "checksum",
                format!(
                    "stored 0x{:08X} computed 0x{computed:08X} ({})",
                    record.checksum,
                    if record.checksum == computed {
                        "match"
                    } else {
                        "mismatch"
                    }
                ),
            ));
            lines.push(line("raw", hexsp(&record.raw, record.raw.len())));
        }
        BondRead::Valid(record) => {
            lines.push(line("valid", "yes"));
            lines.push(line("magic", format!("0x{:08X}", record.magic)));
            lines.push(line("format", record.version));
            lines.push(line("flags", format!("0x{:02X}", record.flags)));
            lines.push(line("session AA", format!("0x{:08X}", record.session_aa)));
            lines.push(ticks_line("interval", record.conn_interval));
            lines.push(ticks_line("timeout", record.conn_timeout));
            lines.push(line("keyboard MAC", format_mac(&record.peer_mac)));
            if record.dongle_mac == [0u8; 6] {
                lines.push(line("dongle MAC", "(none: factory MAC in use)"));
            } else {
                lines.push(line(
                    "dongle MAC",
                    format!("{} (override)", format_mac(&record.dongle_mac)),
                ));
            }
            lines.push(line("reserved", format!("0x{:04X}", record.reserved0)));
            lines.push(line(
                "checksum",
                format!("0x{:08X} (valid)", record.checksum),
            ));
            lines.push(line("raw", hexsp(&record.raw, record.raw.len())));
        }
    }
    lines
}

pub fn show_bond_info(dev: &IapDevice) -> Result<()> {
    let arm = op_arm(dev)?;
    check("GetDevInfo(arm)", &arm, ACK_GETDEVINFO, None)?;

    // Always attempt to disarm, including after a read transport error.
    let read = op_bond_read(dev);
    let disarm = op_disarm(dev);
    let response = read?.ok_or_else(|| anyhow::anyhow!("BondRead: no response (timeout)"))?;
    // Disarm answers with either ack, exactly as in probe(): both mean the
    // write landed. Demanding only ACK_OK would fail a healthy session.
    let disarm = disarm?;
    let dr = disarm
        .as_ref()
        .ok_or_else(|| anyhow::anyhow!("GetDevInfo(disarm): no response (timeout)"))?;
    if dr.is_empty() || (dr[0] != ACK_OK && dr[0] != ACK_GETDEVINFO) {
        anyhow::bail!(
            "GetDevInfo(disarm): unexpected ack; raw={}",
            crate::iap::hexsp(dr, 8)
        );
    }

    for text in render_bond(&decode_response(&response)?) {
        println!("{text}");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid_record() -> [u8; BOND_RECORD_LEN] {
        let mut raw = [0u8; BOND_RECORD_LEN];
        raw[0..4].copy_from_slice(&BOND_MAGIC.to_le_bytes());
        raw[4] = BOND_VERSION;
        raw[5] = 0x02;
        raw[6..8].copy_from_slice(&28u16.to_le_bytes());
        raw[8..12].copy_from_slice(&0xAC12_34CEu32.to_le_bytes());
        raw[12..14].copy_from_slice(&600u16.to_le_bytes());
        raw[16..22].copy_from_slice(&[0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5]);
        raw[22..28].copy_from_slice(&[0x10, 0x20, 0x30, 0x40, 0x50, 0x60]);
        let checksum: u32 = raw[..28].iter().map(|&byte| u32::from(byte)).sum();
        raw[28..32].copy_from_slice(&checksum.to_le_bytes());
        raw
    }

    fn response(status: u8, raw: &[u8; BOND_RECORD_LEN]) -> Vec<u8> {
        let mut response = vec![ACK_BOND_READ, BOND_RECORD_LEN as u8, status];
        response.extend_from_slice(raw);
        response
    }

    fn rendered(response: &[u8]) -> String {
        render_bond(&decode_response(response).unwrap()).join("\n")
    }

    #[test]
    fn decodes_valid_response() {
        let raw = valid_record();
        let BondRead::Valid(record) = decode_response(&response(0, &raw)).unwrap() else {
            panic!("expected valid record");
        };
        assert_eq!(record.magic, BOND_MAGIC);
        assert_eq!(record.session_aa, 0xAC12_34CE);
        assert_eq!(record.conn_interval, 28);
        assert_eq!(record.conn_timeout, 600);
        assert_eq!(record.peer_mac, [0x10, 0x20, 0x30, 0x40, 0x50, 0x60]);
        assert_eq!(record.dongle_mac, [0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5]);
        assert_eq!(format_mac(&record.peer_mac), "10:20:30:40:50:60");
        assert_eq!(record.raw, raw);
    }

    #[test]
    fn accepts_invalid_or_missing_response_as_information() {
        let raw = [0xFF; BOND_RECORD_LEN];
        assert!(matches!(
            decode_response(&response(1, &raw)).unwrap(),
            BondRead::Invalid(record) if record.raw == raw
        ));
    }

    #[test]
    fn rejects_malformed_responses() {
        assert!(decode_response(&[ACK_BOND_READ, BOND_RECORD_LEN as u8]).is_err());

        let mut bad = response(0, &valid_record());
        bad[10] ^= 1;
        assert!(decode_response(&bad).is_err());

        let mut unknown_status = response(0, &valid_record());
        unknown_status[2] = 2;
        assert!(decode_response(&unknown_status).is_err());
    }

    #[test]
    fn valid_record_renders_magic_and_override_mac() {
        let text = rendered(&response(0, &valid_record()));
        assert!(text.contains("valid           yes"), "{text}");
        assert!(text.contains("magic           0x444E4F42"), "{text}");
        assert!(text.contains("format          1"), "{text}");
        assert!(text.contains("flags           0x02"), "{text}");
        assert!(text.contains("session AA      0xAC1234CE"), "{text}");
        assert!(
            text.contains("interval        28 ticks (0.875 ms)"),
            "{text}"
        );
        assert!(
            text.contains("timeout         600 ticks (18.750 ms)"),
            "{text}"
        );
        assert!(text.contains("keyboard MAC    10:20:30:40:50:60"), "{text}");
        assert!(
            text.contains("dongle MAC      A0:A1:A2:A3:A4:A5 (override)"),
            "{text}"
        );
        assert!(text.contains("(valid)"), "{text}");
        assert!(!text.contains("best-effort"), "{text}");
        assert!(text
            .lines()
            .last()
            .unwrap()
            .starts_with("  raw             42 4f 4e 44"));
    }

    #[test]
    fn zero_dongle_mac_means_factory_mac() {
        let mut raw = valid_record();
        raw[16..22].fill(0);
        let checksum: u32 = raw[..28].iter().map(|&byte| u32::from(byte)).sum();
        raw[28..32].copy_from_slice(&checksum.to_le_bytes());
        let text = rendered(&response(0, &raw));
        assert!(
            text.contains("dongle MAC      (none: factory MAC in use)"),
            "{text}"
        );
        assert!(!text.contains("override"), "{text}");
    }

    /// An invalid record is still split into fields, but labelled as a
    /// best-effort reading with the expectations it fails shown alongside.
    #[test]
    fn invalid_record_renders_best_effort_fields() {
        let mut raw = valid_record();
        raw[0] = 0x41; // magic corrupted: "AOND"
        raw[28..32].copy_from_slice(&0x1234_5678u32.to_le_bytes());
        let text = rendered(&response(1, &raw));
        assert!(
            text.contains("valid           no (not present, invalid, or NV read failed)"),
            "{text}"
        );
        assert!(text.contains("best-effort"), "{text}");
        assert!(
            text.contains("magic           0x444E4F41 (expected 0x444E4F42)"),
            "{text}"
        );
        assert!(text.contains("format          1 (expected 1)"), "{text}");
        assert!(text.contains("flags           0x02"), "{text}");
        assert!(text.contains("interval        28 ticks"), "{text}");
        assert!(text.contains("session AA      0xAC1234CE"), "{text}");
        assert!(text.contains("timeout         600 ticks"), "{text}");
        assert!(text.contains("dongle MAC      A0:A1:A2:A3:A4:A5"), "{text}");
        assert!(text.contains("keyboard MAC    10:20:30:40:50:60"), "{text}");
        let computed: u32 = raw[..28].iter().map(|&byte| u32::from(byte)).sum();
        assert!(
            text.contains(&format!(
                "checksum        stored 0x12345678 computed 0x{computed:08X} (mismatch)"
            )),
            "{text}"
        );
        // Not all-zero, so no NV-read hint; and no semantic claims either.
        assert!(!text.contains("may indicate"), "{text}");
        assert!(!text.contains("factory MAC"), "{text}");
        assert!(!text.contains("(valid)"), "{text}");
        assert!(text
            .lines()
            .last()
            .unwrap()
            .starts_with("  raw             41 4f 4e 44"));
    }

    /// All zeros is what a failed NV read produces, but only "may indicate":
    /// the wording must not claim it as proof.
    #[test]
    fn all_zero_invalid_record_hints_at_failed_nv_read() {
        let raw = [0u8; BOND_RECORD_LEN];
        let text = rendered(&response(1, &raw));
        assert!(text.contains("may indicate a failed NV read"), "{text}");
        assert!(text.contains("indistinguishable"), "{text}");
        assert!(
            text.contains("magic           0x00000000 (expected 0x444E4F42)"),
            "{text}"
        );
        assert!(text.contains("dongle MAC      00:00:00:00:00:00"), "{text}");
        assert!(
            text.contains("checksum        stored 0x00000000 computed 0x00000000 (match)"),
            "{text}"
        );
        assert!(text.ends_with(&format!("  raw             {}", hexsp(&raw, raw.len()))));

        let erased = rendered(&response(1, &[0xFF; BOND_RECORD_LEN]));
        assert!(!erased.contains("may indicate"), "{erased}");
    }
}
