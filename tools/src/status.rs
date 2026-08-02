//! Decoder and display for the production status command (0x91).

use anyhow::{bail, Result};

use crate::iap::{hexsp, op_status, IapDevice};

const ACK_STATUS: u8 = 0x91;
const STATUS_SCHEMA: u8 = 1;
const STATUS_LEN: usize = 32;
const UID_LEN: usize = 8;
const MAC_LEN: usize = 6;

#[derive(Debug, PartialEq, Eq)]
pub struct DeviceStatus {
    family: u8,
    connection: u8,
    update: u8,
    capabilities: u8,
    profile: u8,
    uid: [u8; UID_LEN],
    dongle_mac: [u8; MAC_LEN],
    build_id: u32,
    image_len: u32,
}

impl DeviceStatus {
    fn decode(response: &[u8]) -> Result<Self> {
        if response.len() < 2 + STATUS_LEN {
            bail!(
                "Status: short response ({} bytes, expected at least {})",
                response.len(),
                2 + STATUS_LEN
            );
        }
        if response[0] != ACK_STATUS {
            bail!(
                "Status: bad ack 0x{:02X} (want 0x{ACK_STATUS:02X}); raw={}",
                response[0],
                hexsp(response, 8)
            );
        }
        if response[1] as usize != STATUS_LEN {
            bail!(
                "Status: payload length {} (expected {STATUS_LEN})",
                response[1]
            );
        }
        let payload = &response[2..2 + STATUS_LEN];
        if payload[0] != STATUS_SCHEMA {
            bail!("Status: unsupported schema {}", payload[0]);
        }
        if payload[5] as usize != UID_LEN {
            bail!("Status: UID length {} (expected {UID_LEN})", payload[5]);
        }

        let uid: [u8; UID_LEN] = payload[8..16].try_into().unwrap();
        let mut dongle_mac: [u8; MAC_LEN] = payload[24..30].try_into().unwrap();
        // Status v1 initially left this appended field zeroed.
        if dongle_mac.iter().all(|&byte| byte == 0) {
            dongle_mac.copy_from_slice(&uid[..MAC_LEN]);
        }

        Ok(Self {
            family: payload[1],
            connection: payload[2],
            update: payload[3],
            capabilities: payload[4],
            profile: payload[6],
            uid,
            dongle_mac,
            build_id: u32::from_le_bytes(payload[16..20].try_into().unwrap()),
            image_len: u32::from_le_bytes(payload[20..24].try_into().unwrap()),
        })
    }

    fn chip(&self) -> &'static str {
        match self.family {
            0x70 => "CH570",
            0x82 => "CH582",
            0x92 => "CH592",
            _ => "unknown",
        }
    }

    fn profile(&self) -> String {
        match self.profile {
            1 => format!("{}-product", self.chip().to_ascii_lowercase()),
            _ => "unknown".to_string(),
        }
    }

    fn connection(&self) -> &'static str {
        match self.connection {
            0 => "unavailable",
            1 => "pairing",
            2 => "waiting for reconnect",
            3 => "connected",
            _ => "unknown",
        }
    }

    fn update(&self) -> &'static str {
        match self.update {
            0 => "clean",
            1 => "staged",
            2 => "promotion pending",
            0xFF => "unavailable",
            _ => "unknown",
        }
    }
}

fn format_hex(bytes: &[u8], separator: &str) -> String {
    bytes
        .iter()
        .map(|byte| format!("{byte:02X}"))
        .collect::<Vec<_>>()
        .join(separator)
}

pub fn read_status(dev: &IapDevice) -> Result<DeviceStatus> {
    let response =
        op_status(dev)?.ok_or_else(|| anyhow::anyhow!("Status: no response (timeout)"))?;
    DeviceStatus::decode(&response)
}

pub fn show_status(status: &DeviceStatus, firmware_version: &str) {
    println!("status:");
    println!("  chip            {}", status.chip());
    println!("  unique ID       {}", format_hex(&status.uid, ""));
    println!("  dongle MAC      {}", format_hex(&status.dongle_mac, ":"));
    println!("  firmware        {firmware_version}");
    println!(
        "  build           {} / {:08X}",
        status.profile(),
        status.build_id
    );
    println!("  image           {} bytes", status.image_len);
    println!("  update          {}", status.update());
    println!("  connection      {}", status.connection());
    println!("  capabilities    0x{:02X}", status.capabilities);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn response() -> Vec<u8> {
        let mut response = vec![ACK_STATUS, STATUS_LEN as u8];
        let mut payload = [0u8; STATUS_LEN];
        payload[0] = STATUS_SCHEMA;
        payload[1] = 0x70;
        payload[2] = 3;
        payload[3] = 0;
        payload[4] = 0x0F;
        payload[5] = UID_LEN as u8;
        payload[6] = 1;
        payload[8..16].copy_from_slice(&[0xB2, 0x59, 0x21, 0x62, 0x32, 0xDC, 5, 0x98]);
        payload[16..20].copy_from_slice(&0x1234_ABCDu32.to_le_bytes());
        payload[20..24].copy_from_slice(&25_848u32.to_le_bytes());
        payload[24..30].copy_from_slice(&[0xBA, 0x47, 0x8B, 0x72, 0xAB, 0x3C]);
        response.extend_from_slice(&payload);
        response
    }

    #[test]
    fn decodes_status_v1() {
        let status = DeviceStatus::decode(&response()).unwrap();
        assert_eq!(status.chip(), "CH570");
        assert_eq!(status.profile(), "ch570-product");
        assert_eq!(status.connection(), "connected");
        assert_eq!(status.update(), "clean");
        assert_eq!(status.build_id, 0x1234_ABCD);
        assert_eq!(status.image_len, 25_848);
        assert_eq!(format_hex(&status.dongle_mac, ":"), "BA:47:8B:72:AB:3C");
    }

    #[test]
    fn falls_back_to_uid_for_legacy_status() {
        let mut response = response();
        response[2 + 24..2 + 30].fill(0);
        let status = DeviceStatus::decode(&response).unwrap();
        assert_eq!(format_hex(&status.dongle_mac, ":"), "B2:59:21:62:32:DC");
    }

    #[test]
    fn rejects_malformed_status() {
        assert!(DeviceStatus::decode(&[ACK_STATUS, STATUS_LEN as u8]).is_err());
        let mut wrong_schema = response();
        wrong_schema[2] = 2;
        assert!(DeviceStatus::decode(&wrong_schema).is_err());
        let mut wrong_uid = response();
        wrong_uid[7] = 6;
        assert!(DeviceStatus::decode(&wrong_uid).is_err());
    }
}
