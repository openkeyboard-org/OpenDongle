//! USB-HID IAP transport and protocol for the OpenDongle dongle (WCH CH592F).
//!
//! Direct port of the transport/protocol layer in `flash_dongle.py`. The on-wire
//! framing is identical: a 65-byte HID report carrying a leading 0x00 report-ID
//! byte, then `[cmd][len][body...][checksum]` zero-padded to 65. hidapi expects
//! the report-ID byte as `buf[0]` on write and strips it on read, so response
//! buffers are the 64-byte payload (ACK at index 0), matching the Python which
//! reads from hidraw after the kernel strips the report ID.

use std::ffi::CString;
use std::time::Duration;

use anyhow::{anyhow, bail, Result};
use hidapi::{DeviceInfo, HidApi, HidDevice};

// ---------------- USB / HID constants ----------------

pub const DEFAULT_VID: u16 = 0x0C45; // observed on a production Bridge75
pub const DEFAULT_PID: u16 = 0xFEFE;
pub const IAP_INTERFACE: i32 = 4; // `mi_04` — 64-byte EP0x06/0x86
pub const REPORT_SIZE: usize = 65; // 1 Report-ID byte + 64-byte payload

// ---------------- IAP protocol constants ----------------

pub const CMD_HANDSHAKE: u8 = 0x5A; // with "WCH@HFD" payload
pub const CMD_GETDEVINFO: u8 = 0x84;
pub const CMD_BOND_READ: u8 = 0x88;
pub const CMD_VERSION: u8 = 0x90;
pub const CMD_STATUS: u8 = 0x91;
pub const CMD_FAULT: u8 = 0x93;
/// Reboot into the OpenBoot bootloader (armed session only). Body is the
/// 4-byte ENTER_BOOTLOADER_MAGIC, little-endian; the device replies status
/// 0, quiesces RF, drains the reply, and resets into OpenBoot.
pub const CMD_ENTER_BOOTLOADER: u8 = 0x85;
/// OB_BOOTREQ_MAGIC from OpenBoot's protocol header.
pub const ENTER_BOOTLOADER_MAGIC: u32 = 0xB007_CA11;

pub const HANDSHAKE_PAYLOAD: &[u8] = b"WCH@HFD"; // see firmware VA 0x654C; 7 bytes

pub const ACK_HANDSHAKE: u8 = 0xA5;
pub const ACK_GETDEVINFO: u8 = 0x04;
pub const ACK_OK: u8 = 0x0F;

pub const POST_WRITE_SLEEP: Duration = Duration::from_millis(1); // Windows tool sleeps 1 ms WR/RD
pub const READ_TIMEOUT_MS: i32 = 1000;

/// A response is the 64-byte payload (report ID already stripped), or `None` on
/// timeout — mirroring `IAPDevice.xfer` returning `None`.
pub type Response = Option<Vec<u8>>;

// ---------------- Packet layer ----------------

/// Build a `[0x00][cmd][len][body...][checksum]` report, zero-padded to
/// `REPORT_SIZE`. `checksum = (cmd + len + sum(body)) & 0xFF`. Ports `_packet`.
fn build_packet(cmd: u8, body: &[u8]) -> [u8; REPORT_SIZE] {
    assert!(
        body.len() <= REPORT_SIZE - 4,
        "body too large for one report"
    );
    let mut pkt = [0u8; REPORT_SIZE];
    pkt[0] = 0x00; // HID report ID
    pkt[1] = cmd;
    pkt[2] = body.len() as u8;
    pkt[3..3 + body.len()].copy_from_slice(body);
    let mut checksum = cmd.wrapping_add(body.len() as u8);
    for &b in body {
        checksum = checksum.wrapping_add(b);
    }
    pkt[3 + body.len()] = checksum;
    pkt
}

/// Plain `[cmd][len][data][cksum]` packet. Ports `packet_simple`.
pub fn packet_simple(cmd: u8, data: &[u8]) -> [u8; REPORT_SIZE] {
    build_packet(cmd, data)
}

/// EnterBootloader packet: `[0x85][4][11 CA 07 B0][cksum]`. The magic payload
/// keeps a stray/fuzzed report from resetting the dongle.
pub fn packet_enter_bootloader() -> [u8; REPORT_SIZE] {
    build_packet(CMD_ENTER_BOOTLOADER, &ENTER_BOOTLOADER_MAGIC.to_le_bytes())
}

// ---------------- HID transport ----------------

/// Find the HID interface for (vid, pid, interface). On macOS `interface_number`
/// can be -1 for composite devices; a usage-page fallback may be needed there
/// (verify on hardware). Linux/Windows report the interface number reliably.
fn find_device(api: &HidApi, vid: u16, pid: u16, interface: i32) -> Option<&DeviceInfo> {
    api.device_list().find(|d| {
        d.vendor_id() == vid && d.product_id() == pid && d.interface_number() == interface
    })
}

pub struct IapDevice {
    dev: HidDevice,
    /// Human-readable device path for logging.
    pub path: String,
}

impl IapDevice {
    /// Open the IAP device, draining any stale input reports (ports the
    /// non-blocking drain loop in `IAPDevice.__enter__`).
    pub fn open(
        api: &HidApi,
        vid: u16,
        pid: u16,
        interface: i32,
        explicit_path: Option<&str>,
    ) -> Result<IapDevice> {
        let (dev, path) = if let Some(p) = explicit_path {
            let cpath = CString::new(p)?;
            (api.open_path(&cpath)?, p.to_string())
        } else {
            let info = find_device(api, vid, pid, interface).ok_or_else(|| {
                anyhow!("no HID device for VID=0x{vid:04X} PID=0x{pid:04X} interface={interface}")
            })?;
            let path = info.path().to_string_lossy().into_owned();
            (info.open_device(api)?, path)
        };

        // Drain any buffered input reports so the first xfer sees a fresh reply.
        dev.set_blocking_mode(false).ok();
        let mut buf = [0u8; REPORT_SIZE];
        while let Ok(n) = dev.read_timeout(&mut buf, 0) {
            if n == 0 {
                break;
            }
        }
        Ok(IapDevice { dev, path })
    }

    /// Write a report, wait, and read one response (or `None` on timeout).
    /// Ports `IAPDevice.xfer`.
    pub fn xfer(&self, packet: &[u8], timeout_ms: i32) -> Result<Response> {
        assert_eq!(packet.len(), REPORT_SIZE);
        self.dev.write(packet)?;
        std::thread::sleep(POST_WRITE_SLEEP);
        let mut buf = [0u8; REPORT_SIZE];
        let n = self.dev.read_timeout(&mut buf, timeout_ms)?;
        if n == 0 {
            return Ok(None);
        }
        Ok(Some(buf[..n].to_vec()))
    }
}

// ---------------- High-level IAP operations ----------------

pub fn op_handshake(dev: &IapDevice) -> Result<Response> {
    dev.xfer(
        &packet_simple(CMD_HANDSHAKE, HANDSHAKE_PAYLOAD),
        READ_TIMEOUT_MS,
    )
}

pub fn op_arm(dev: &IapDevice) -> Result<Response> {
    dev.xfer(
        &packet_simple(CMD_GETDEVINFO, &[0x01, 0x00, 0x00, 0x00]),
        READ_TIMEOUT_MS,
    )
}

pub fn op_disarm(dev: &IapDevice) -> Result<Response> {
    dev.xfer(
        &packet_simple(CMD_GETDEVINFO, &[0x00, 0x00, 0x00, 0x00]),
        READ_TIMEOUT_MS,
    )
}

/// Request the reboot into OpenBoot (armed session only). The device sends
/// the status-0 reply, quiesces RF, drains EP6-IN, and resets; expect it to
/// drop off the bus shortly after this returns.
pub fn op_enter_bootloader(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_enter_bootloader(), READ_TIMEOUT_MS)
}

pub fn op_version(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_VERSION, &[]), READ_TIMEOUT_MS)
}

pub fn op_bond_read(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_BOND_READ, &[]), READ_TIMEOUT_MS)
}

pub fn op_status(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_STATUS, &[]), READ_TIMEOUT_MS)
}

pub fn op_fault(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_FAULT, &[]), READ_TIMEOUT_MS)
}

// ---------------- Response checking ----------------

/// Format the first `n` bytes of a response like Python's `r[:8].hex(' ')`.
pub fn hexsp(r: &[u8], n: usize) -> String {
    r.iter()
        .take(n)
        .map(|b| format!("{b:02x}"))
        .collect::<Vec<_>>()
        .join(" ")
}

/// Validate an IAP response and error on failure. `expected_status = None`
/// skips the status-byte check. Ports `_check`.
pub fn check(
    name: &str,
    r: &Response,
    expected_ack: u8,
    expected_status: Option<u8>,
) -> Result<()> {
    let r = r
        .as_ref()
        .ok_or_else(|| anyhow!("{name}: no response (timeout)"))?;
    if r[0] != expected_ack {
        bail!(
            "{name}: bad ack 0x{:02X} (want 0x{:02X}); raw={}",
            r[0],
            expected_ack,
            hexsp(r, 8)
        );
    }
    if let Some(st) = expected_status {
        if r[2] != st {
            bail!(
                "{name}: status byte 0x{:02X} != 0; raw={}",
                r[2],
                hexsp(r, 8)
            );
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn to_hex(b: &[u8]) -> String {
        b.iter().map(|x| format!("{x:02x}")).collect()
    }

    // Golden vectors generated from flash_dongle.py (65-byte reports).
    #[test]
    fn packet_simple_golden() {
        assert_eq!(
            to_hex(&packet_simple(CMD_HANDSHAKE, HANDSHAKE_PAYLOAD)),
            "005a075743484048464455000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
        assert_eq!(
            to_hex(&packet_simple(CMD_GETDEVINFO, &[0x01, 0x00, 0x00, 0x00])),
            "0084040100000089000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
        assert_eq!(
            to_hex(&packet_simple(CMD_GETDEVINFO, &[0x00, 0x00, 0x00, 0x00])),
            "0084040000000088000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
        assert_eq!(
            to_hex(&packet_simple(CMD_VERSION, &[])),
            "0090009000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
        assert_eq!(
            to_hex(&packet_simple(CMD_BOND_READ, &[])),
            "0088008800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
        assert_eq!(
            to_hex(&packet_simple(CMD_STATUS, &[])),
            "0091009100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
        assert_eq!(
            to_hex(&packet_simple(CMD_FAULT, &[])),
            "0093009300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
        // enter-bootloader body: OB_BOOTREQ_MAGIC 0xB007CA11 little-endian
        assert_eq!(
            to_hex(&packet_enter_bootloader()),
            "00850411ca07b01b000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        );
    }

    #[test]
    fn crc32_matches_zlib() {
        let r64: Vec<u8> = (0u8..64).collect();
        assert_eq!(crc32fast::hash(&r64), 0x100e_ce8c);
        assert_eq!(crc32fast::hash(b""), 0x0000_0000);
        assert_eq!(crc32fast::hash(HANDSHAKE_PAYLOAD), 0xa85e_8a42);
    }

}
