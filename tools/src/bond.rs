//! Read-only decoding and display for the firmware's persistent bond record.

use anyhow::{bail, Result};

use crate::iap::{
    check, hexsp, op_arm, op_bond_read, op_disarm, IapDevice, ACK_GETDEVINFO, ACK_OK,
};

const BOND_MAGIC: u32 = 0x444E_4F42;
const BOND_VERSION: u8 = 2;
const BOND_RECORD_LEN: usize = 48;
const ACK_BOND_READ: u8 = 0x88;

const BOND_FLAG_ENC_CAPABLE: u8 = 0x01;
const BOND_FLAG_ENC_KEY: u8 = 0x02;

// BondRead validity byte (response[2]) bits.
const BOND_READ_INVALID: u8 = 0x01;
const BOND_READ_REDACTED: u8 = 0x02;

#[derive(Debug, PartialEq, Eq)]
struct BondRecord {
    version: u8,
    flags: u8,
    conn_interval: u16,
    session_aa: u32,
    conn_timeout: u16,
    reserved0: u16,
    peer_mac: [u8; 6],
    checksum: u32,
    // The firmware zeroes link_key in the BondRead response and sets the
    // redacted bit; the key is never transported. This records that the device
    // held one, for display only.
    redacted: bool,
}

enum BondRead {
    Valid(BondRecord),
    Invalid([u8; BOND_RECORD_LEN]),
}

impl BondRecord {
    fn decode(raw: &[u8; BOND_RECORD_LEN], redacted: bool) -> Result<Self> {
        let magic = u32::from_le_bytes(raw[0..4].try_into().unwrap());
        let record = Self {
            version: raw[4],
            flags: raw[5],
            conn_interval: u16::from_le_bytes(raw[6..8].try_into().unwrap()),
            session_aa: u32::from_le_bytes(raw[8..12].try_into().unwrap()),
            conn_timeout: u16::from_le_bytes(raw[12..14].try_into().unwrap()),
            reserved0: u16::from_le_bytes(raw[14..16].try_into().unwrap()),
            peer_mac: raw[22..28].try_into().unwrap(),
            checksum: u32::from_le_bytes(raw[44..48].try_into().unwrap()),
            redacted,
        };
        let expected_checksum: u32 = raw[..44].iter().map(|&byte| u32::from(byte)).sum();

        if magic != BOND_MAGIC {
            bail!("BondRead: firmware marked a record valid with bad magic 0x{magic:08X}");
        }
        if record.version != BOND_VERSION {
            bail!(
                "BondRead: firmware marked unsupported bond version {} valid",
                record.version
            );
        }
        if record.session_aa == 0 {
            bail!("BondRead: firmware marked a zero session address valid");
        }
        if record.checksum != expected_checksum {
            bail!(
                "BondRead: firmware marked a record valid with checksum 0x{:08X} (expected 0x{expected_checksum:08X})",
                record.checksum
            );
        }
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
    let status = response[2];
    if status & !(BOND_READ_INVALID | BOND_READ_REDACTED) != 0 {
        bail!("BondRead: unknown validity status 0x{status:02X}");
    }
    if status & BOND_READ_INVALID != 0 {
        return Ok(BondRead::Invalid(raw));
    }
    let redacted = status & BOND_READ_REDACTED != 0;
    Ok(BondRead::Valid(BondRecord::decode(&raw, redacted)?))
}

fn format_mac(mac: &[u8; 6]) -> String {
    mac.iter()
        .map(|byte| format!("{byte:02X}"))
        .collect::<Vec<_>>()
        .join(":")
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

    println!("bond:");
    match decode_response(&response)? {
        BondRead::Invalid(raw) => {
            println!("  valid           no (not present or invalid)");
            println!("  raw             {}", hexsp(&raw, raw.len()));
        }
        BondRead::Valid(record) => {
            let capable = record.flags & BOND_FLAG_ENC_CAPABLE != 0;
            let has_key = record.flags & BOND_FLAG_ENC_KEY != 0;
            let encryption = match (capable, has_key) {
                (true, true) => "active (capable + key)",
                (true, false) => "off (capable, no key)",
                (false, true) => "off (key, peer not capable)",
                (false, false) => "off (plaintext)",
            };
            println!("  valid           yes");
            println!("  format          {}", record.version);
            println!("  flags           0x{:02X}", record.flags);
            println!("  encryption      {encryption}");
            if has_key {
                let key = if record.redacted { "present (redacted)" } else { "present" };
                println!("  link key        {key}");
            }
            println!("  session AA      0x{:08X}", record.session_aa);
            println!(
                "  interval        {} ticks ({:.3} ms)",
                record.conn_interval,
                f64::from(record.conn_interval) / 32.0
            );
            println!(
                "  timeout         {} ticks ({:.3} ms)",
                record.conn_timeout,
                f64::from(record.conn_timeout) / 32.0
            );
            println!("  keyboard MAC    {}", format_mac(&record.peer_mac));
            println!("  reserved        0x{:04X}", record.reserved0);
            println!("  checksum        0x{:08X} (valid)", record.checksum);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn record_with_flags(flags: u8) -> [u8; BOND_RECORD_LEN] {
        let mut raw = [0u8; BOND_RECORD_LEN];
        raw[0..4].copy_from_slice(&BOND_MAGIC.to_le_bytes());
        raw[4] = BOND_VERSION;
        raw[5] = flags;
        raw[6..8].copy_from_slice(&28u16.to_le_bytes());
        raw[8..12].copy_from_slice(&0xAC12_34CEu32.to_le_bytes());
        raw[12..14].copy_from_slice(&600u16.to_le_bytes());
        raw[16..22].copy_from_slice(&[0, 0, 0, 0, 0, 0]);
        raw[22..28].copy_from_slice(&[0x10, 0x20, 0x30, 0x40, 0x50, 0x60]);
        // link_key (raw[28..44]) left zero: matches a redacted-view checksum.
        let checksum: u32 = raw[..44].iter().map(|&byte| u32::from(byte)).sum();
        raw[44..48].copy_from_slice(&checksum.to_le_bytes());
        raw
    }

    fn valid_record() -> [u8; BOND_RECORD_LEN] {
        record_with_flags(0x00)
    }

    #[test]
    fn decodes_valid_response() {
        let raw = valid_record();
        let mut response = vec![ACK_BOND_READ, BOND_RECORD_LEN as u8, 0];
        response.extend_from_slice(&raw);

        let BondRead::Valid(record) = decode_response(&response).unwrap() else {
            panic!("expected valid record");
        };
        assert_eq!(record.version, 2);
        assert_eq!(record.session_aa, 0xAC12_34CE);
        assert_eq!(record.conn_interval, 28);
        assert_eq!(record.conn_timeout, 600);
        assert_eq!(record.peer_mac, [0x10, 0x20, 0x30, 0x40, 0x50, 0x60]);
        assert_eq!(format_mac(&record.peer_mac), "10:20:30:40:50:60");
        assert!(!record.redacted);
    }

    #[test]
    fn decodes_redacted_encrypted_response() {
        // ENC_CAPABLE | ENC_KEY, key zeroed by the firmware, redacted bit set.
        let raw = record_with_flags(BOND_FLAG_ENC_CAPABLE | BOND_FLAG_ENC_KEY);
        let mut response =
            vec![ACK_BOND_READ, BOND_RECORD_LEN as u8, BOND_READ_REDACTED];
        response.extend_from_slice(&raw);

        let BondRead::Valid(record) = decode_response(&response).unwrap() else {
            panic!("expected valid record");
        };
        assert!(record.redacted);
        assert_eq!(record.flags, BOND_FLAG_ENC_CAPABLE | BOND_FLAG_ENC_KEY);
    }

    #[test]
    fn accepts_invalid_or_missing_response_as_information() {
        let raw = [0xFF; BOND_RECORD_LEN];
        let mut response = vec![ACK_BOND_READ, BOND_RECORD_LEN as u8, 1];
        response.extend_from_slice(&raw);
        assert!(matches!(
            decode_response(&response).unwrap(),
            BondRead::Invalid(bytes) if bytes == raw
        ));
    }

    #[test]
    fn rejects_malformed_responses() {
        assert!(decode_response(&[ACK_BOND_READ, BOND_RECORD_LEN as u8]).is_err());

        let raw = valid_record();
        let mut response = vec![ACK_BOND_READ, BOND_RECORD_LEN as u8, 0];
        response.extend_from_slice(&raw);
        response[10] ^= 1;
        assert!(decode_response(&response).is_err());
    }
}
