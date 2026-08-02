//! High-level flows. Since the OpenBoot cutover the vendor IAP interface no
//! longer flashes: `probe` reads device identity, and updates happen in the
//! OpenBoot bootloader over OBP (`opendongle --enter-bootloader`, then the
//! `openboot` CLI).

use anyhow::{anyhow, bail, Result};

use crate::iap::*;

pub struct DevInfo {
    /// Formerly the staging base; the firmware reports 0 since the OpenBoot
    /// cutover (field retained so the response shape never moved).
    pub reserved_geometry: u32,
    /// Formerly the program block size; 0 since the OpenBoot cutover.
    pub reserved_block: u16,
    pub chip_id: u16,
    #[allow(dead_code)]
    pub chip_extra: u16,
}

/// Handshake + GetDevInfo read. Leaves the session disarmed. Ports `probe`.
pub fn probe(dev: &IapDevice) -> Result<DevInfo> {
    let r = op_handshake(dev)?;
    check("Handshake", &r, ACK_HANDSHAKE, None)?;
    let rr = r.as_ref().unwrap();
    println!(
        "  handshake       ack=0x{:02X}  raw={}",
        rr[0],
        hexsp(rr, 8)
    );

    let r = op_arm(dev)?;
    check("GetDevInfo(arm)", &r, ACK_GETDEVINFO, None)?;
    let rr = r.as_ref().unwrap();
    let info = DevInfo {
        reserved_geometry: u32::from_le_bytes([rr[2], rr[3], rr[4], rr[5]]),
        reserved_block: u16::from_le_bytes([rr[6], rr[7]]),
        chip_id: u16::from_le_bytes([rr[8], rr[9]]),
        chip_extra: u16::from_le_bytes([rr[10], rr[11]]),
    };
    let family = match info.chip_id & 0xFF {
        0x92 => "CH592 family",
        0x82 => "CH582 family",
        0x70 => "CH570 family",
        _ => "unexpected",
    };
    println!(
        "  GetDevInfo(1)   ack=0x{:02X}  raw={}",
        rr[0],
        hexsp(rr, 14)
    );
    if info.reserved_geometry != 0 || info.reserved_block != 0 {
        // Pre-OpenBoot firmware still advertises its staging geometry here.
        println!(
            "    staging_base  = 0x{:08X}  (pre-OpenBoot firmware)",
            info.reserved_geometry
        );
        println!("    block_size    = 0x{:04X}", info.reserved_block);
    }
    println!(
        "    chip_id byte  = 0x{:02X}  ({family})",
        info.chip_id & 0xFF
    );

    // Leave the session in a known state. Either ack means the write landed.
    let r = op_disarm(dev)?;
    let rr = r
        .as_ref()
        .ok_or_else(|| anyhow!("GetDevInfo(disarm): no response (timeout)"))?;
    if rr[0] != ACK_OK && rr[0] != ACK_GETDEVINFO {
        bail!(
            "GetDevInfo(disarm): unexpected ack 0x{:02X}; raw={}",
            rr[0],
            hexsp(rr, 8)
        );
    }
    Ok(info)
}

/// Armed enter-bootloader sequence: handshake, arm, 0x85 with the magic
/// payload, expecting the status-0 reply before the device resets into
/// OpenBoot. The caller watches for the device leaving the bus.
pub fn enter_bootloader(dev: &IapDevice) -> Result<()> {
    let r = op_handshake(dev)?;
    check("Handshake", &r, ACK_HANDSHAKE, None)?;
    let r = op_arm(dev)?;
    check("GetDevInfo(arm)", &r, ACK_GETDEVINFO, None)?;
    let r = op_enter_bootloader(dev)?;
    let rr = r
        .as_ref()
        .ok_or_else(|| anyhow!("EnterBootloader: no response (timeout)"))?;
    // Reply is the status4 shape [ACK_OK, 0x01, status, 0x00] — validate all
    // four bytes, not just the ack and status.
    if rr.len() < 4
        || rr[0] != ACK_OK
        || rr[1] != 0x01
        || rr[2] != 0
        || rr[3] != 0
    {
        bail!(
            "EnterBootloader: refused; ack=0x{:02X} status=0x{:02X} raw={}",
            rr[0],
            rr.get(2).copied().unwrap_or(0xFF),
            hexsp(rr, 8)
        );
    }
    println!("  EnterBootloader accepted; device is resetting into OpenBoot");
    Ok(())
}
