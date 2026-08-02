//! Decoder and concise display for the retained fault record (command 0x93).

use anyhow::{bail, Result};

use crate::iap::{hexsp, op_fault, IapDevice};

const ACK_FAULT: u8 = 0x93;
const FAULT_LEN: usize = 40;

#[derive(Debug, PartialEq, Eq)]
struct FaultStatus {
    valid: bool,
    kind: u8,
    action: u8,
    flags: u8,
    event_count: u32,
    reset_request_count: u32,
    repeat_count: u32,
    boot_count: u32,
    mcause: u32,
    mepc: u32,
    mtval: u32,
    reset_status: u8,
}

impl FaultStatus {
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
        if payload[1] == 0 {
            bail!("FaultRead: invalid page schema 0");
        }
        Ok(Self {
            valid: payload[2] != 0,
            kind: payload[4],
            action: payload[5],
            flags: payload[6],
            event_count: u32::from_le_bytes(payload[8..12].try_into().unwrap()),
            reset_request_count: u32::from_le_bytes(payload[12..16].try_into().unwrap()),
            repeat_count: u32::from_le_bytes(payload[16..20].try_into().unwrap()),
            boot_count: u32::from_le_bytes(payload[20..24].try_into().unwrap()),
            mcause: u32::from_le_bytes(payload[24..28].try_into().unwrap()),
            mepc: u32::from_le_bytes(payload[28..32].try_into().unwrap()),
            mtval: u32::from_le_bytes(payload[32..36].try_into().unwrap()),
            reset_status: payload[38] & 0x07,
        })
    }

    fn reset_name(&self) -> &'static str {
        match self.reset_status {
            0 => "software",
            1 => "power-on",
            2 => "watchdog",
            3 => "external",
            5 => "wake",
            _ => "unknown",
        }
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

pub fn show_fault_info(dev: &IapDevice) -> Result<()> {
    let response =
        op_fault(dev)?.ok_or_else(|| anyhow::anyhow!("FaultRead: no response (timeout)"))?;
    let fault = FaultStatus::decode(&response)?;

    println!("health:");
    println!(
        "  last reset      {} (0x{:02X})",
        fault.reset_name(),
        fault.reset_status
    );
    if !fault.valid {
        println!("  boot count      unavailable");
        println!("  last fault      unavailable");
    } else {
        println!("  boot count      {}", fault.boot_count);
        println!("  last fault      {}", fault.fault_name());
        if fault.kind != 0 || fault.action != 0 || fault.flags != 0 {
            println!("  action          {}", fault.action_name());
            println!("  flags           0x{:02X}", fault.flags);
            println!(
                "  counts          events={} reset-requests={} repeats={}",
                fault.event_count, fault.reset_request_count, fault.repeat_count
            );
            println!("  mcause          0x{:08X}", fault.mcause);
            println!("  mepc            0x{:08X}", fault.mepc);
            println!("  mtval           0x{:08X}", fault.mtval);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn response() -> Vec<u8> {
        let mut response = vec![ACK_FAULT, FAULT_LEN as u8];
        let mut payload = [0u8; FAULT_LEN];
        payload[0] = 0x70;
        payload[1] = 5;
        payload[2] = 1;
        payload[3] = 1;
        payload[20..24].copy_from_slice(&3u32.to_le_bytes());
        payload[38] = 1;
        response.extend_from_slice(&payload);
        response
    }

    #[test]
    fn decodes_healthy_record() {
        let fault = FaultStatus::decode(&response()).unwrap();
        assert!(fault.valid);
        assert_eq!(fault.boot_count, 3);
        assert_eq!(fault.reset_name(), "power-on");
        assert_eq!(fault.fault_name(), "none");
    }

    #[test]
    fn decodes_fault_details() {
        let mut response = response();
        response[2 + 4] = 0xDE;
        response[2 + 5] = 2;
        response[2 + 8..2 + 12].copy_from_slice(&1u32.to_le_bytes());
        response[2 + 24..2 + 28].copy_from_slice(&2u32.to_le_bytes());
        response[2 + 28..2 + 32].copy_from_slice(&0x1234u32.to_le_bytes());
        let fault = FaultStatus::decode(&response).unwrap();
        assert_eq!(fault.fault_name(), "hard fault");
        assert_eq!(fault.action_name(), "recovered");
        assert_eq!(fault.event_count, 1);
        assert_eq!(fault.mcause, 2);
        assert_eq!(fault.mepc, 0x1234);
    }

    #[test]
    fn rejects_malformed_fault_response() {
        assert!(FaultStatus::decode(&[ACK_FAULT, FAULT_LEN as u8]).is_err());
        let mut wrong_length = response();
        wrong_length[1] = 39;
        assert!(FaultStatus::decode(&wrong_length).is_err());
    }
}
