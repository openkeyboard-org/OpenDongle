// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! USB-HID IAP transport and protocol for the OpenDongle dongle (WCH CH570/CH592).
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
pub const CMD_BOND_WRITE: u8 = 0x87;
pub const CMD_BOND_READ: u8 = 0x88;
pub const CMD_BOND_CLEAR: u8 = 0x89;
pub const CMD_VERSION: u8 = 0x90;
pub const CMD_STATUS: u8 = 0x91;
pub const CMD_RF_DIAG: u8 = 0x92;
pub const CMD_RF_POKE: u8 = 0x94;
pub const CMD_FAULT: u8 = 0x93;
pub const CMD_CRYPT_DIAG: u8 = 0x98;
/// A 32-byte entropy seed for the next link-key exchange. The keyboard's radio
/// module takes the identical packet from its MCU over the UART.
pub const CMD_KEX_SEED: u8 = 0x9C;
pub const KEX_SEED_BYTES: usize = 32;
/// Reboot into the OpenBoot bootloader (armed session only). Body is the
/// 4-byte ENTER_BOOTLOADER_MAGIC, little-endian; the device replies status
/// 0, quiesces RF, drains the reply, and resets into OpenBoot.
pub const CMD_ENTER_BOOTLOADER: u8 = 0x85;
/// OB_BOOTREQ_MAGIC from OpenBoot's protocol header.
pub const ENTER_BOOTLOADER_MAGIC: u32 = 0xB007_CA11;

pub const HANDSHAKE_PAYLOAD: &[u8] = b"WCH@HFD"; // see firmware VA 0x654C; 7 bytes

pub const ACK_HANDSHAKE: u8 = 0xA5;
/// Bytes the GetDevInfo reply must carry before `flows::probe` can decode it:
/// the ack, a status byte, then the 10-byte identity body it reads through.
pub const DEVINFO_LEN: usize = 12;

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

/// The usage the application declares on its vendor interfaces
/// (firmware/common/src/usb_descriptors.c). Interfaces 3 and 4 BOTH declare
/// it, so the usage alone never identifies the IAP interface: 3 is a 32-byte
/// input-only pipe, 4 is the 64-byte in/out one this tool talks to.
pub const IAP_USAGE_PAGE: u16 = 0xFF60;
pub const IAP_USAGE: u16 = 0x0061;

/// Whether a HID report descriptor declares an Output report.
///
/// This is what tells interface 4 from interface 3 where the platform does
/// not number interfaces: only the IAP interface can be written to. Walks the
/// items rather than scanning for a byte, since `0x91` can also be data.
pub fn has_output_report(descriptor: &[u8]) -> bool {
    let mut at = 0;
    while at < descriptor.len() {
        let prefix = descriptor[at];
        if prefix == 0xFE {
            // Long item: [0xFE][data size][tag][data...].
            let Some(&size) = descriptor.get(at + 1) else {
                return false;
            };
            at += 3 + usize::from(size);
            continue;
        }
        // Short item: tag in the high nibble, type in bits 2-3, and a size
        // code in bits 0-1 where 3 means four bytes. Main item (type 0) with
        // tag 9 is Output.
        if prefix & 0xFC == 0x90 {
            return true;
        }
        at += 1 + [0usize, 1, 2, 4][usize::from(prefix & 0x03)];
    }
    false
}

/// How to pick the IAP interface out of one VID:PID's HID interfaces.
#[derive(Debug, PartialEq, Eq)]
enum Pick {
    /// This entry carries the wanted interface number.
    Entry(usize),
    /// The platform reports no interface numbers. These entries declare the
    /// IAP usage, and only their report descriptors can say which is which.
    AskDescriptors(Vec<usize>),
    Nothing,
}

/// `entries` is `(interface number, usage page, usage)` for every HID
/// interface on the VID:PID, in enumeration order.
///
/// An interface number, where the platform gives one, is authoritative and
/// is all that is consulted. The usage fallback applies only when NO entry is
/// numbered (macOS reports -1 for some composite devices) and only for the
/// default IAP interface — an explicit `--interface N` on such a platform
/// cannot be honoured and finds nothing, rather than being quietly replaced.
fn pick_interface(entries: &[(i32, u16, u16)], interface: i32) -> Pick {
    if let Some(at) = entries.iter().position(|e| e.0 == interface) {
        return Pick::Entry(at);
    }
    if interface != IAP_INTERFACE || entries.is_empty() || entries.iter().any(|e| e.0 >= 0) {
        return Pick::Nothing;
    }
    let candidates: Vec<usize> = entries
        .iter()
        .enumerate()
        .filter(|(_, e)| (e.1, e.2) == (IAP_USAGE_PAGE, IAP_USAGE))
        .map(|(at, _)| at)
        .collect();
    if candidates.is_empty() {
        Pick::Nothing
    } else {
        Pick::AskDescriptors(candidates)
    }
}

/// Open each candidate and keep the one whose descriptor has an Output
/// report. Opening sends nothing. Anything but exactly one is a refusal: with
/// none the descriptors could not be read, and with several (two dongles)
/// nothing says which was meant.
fn open_by_descriptor(api: &HidApi, candidates: &[&DeviceInfo]) -> Result<(HidDevice, String)> {
    let mut found = Vec::new();
    for info in candidates {
        let Ok(dev) = info.open_device(api) else {
            continue;
        };
        let mut descriptor = [0u8; hidapi::MAX_REPORT_DESCRIPTOR_SIZE];
        let Ok(len) = dev.get_report_descriptor(&mut descriptor) else {
            continue;
        };
        if has_output_report(&descriptor[..len]) {
            found.push((dev, info.path().to_string_lossy().into_owned()));
        }
    }
    if found.len() > 1 {
        bail!(
            "{} interfaces look like the IAP interface and this platform does not \
             number them, so they cannot be told apart; attach one dongle, or \
             name the device with --hidraw",
            found.len()
        );
    }
    found.pop().ok_or_else(|| {
        anyhow!(
            "this platform does not number HID interfaces, and none of the {} \
             with the dongle's vendor usage could be identified as the IAP \
             interface; name the device with --hidraw",
            candidates.len()
        )
    })
}
/// The pipe an [`IapDevice`] talks through: one report out, one report in.
/// `HidDevice` is the real one; tests script their own.
pub trait ReportLink {
    /// Write one `REPORT_SIZE`-byte report (leading report-ID byte included).
    fn write_report(&self, report: &[u8]) -> Result<()>;
    /// Read one report into `buf`; `Ok(0)` when nothing arrived in time.
    fn read_report(&self, buf: &mut [u8], timeout_ms: i32) -> Result<usize>;
}

impl ReportLink for HidDevice {
    fn write_report(&self, report: &[u8]) -> Result<()> {
        self.write(report)?;
        Ok(())
    }

    fn read_report(&self, buf: &mut [u8], timeout_ms: i32) -> Result<usize> {
        Ok(self.read_timeout(buf, timeout_ms)?)
    }
}

pub struct IapDevice {
    link: Box<dyn ReportLink>,
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
            let entries: Vec<&DeviceInfo> = api
                .device_list()
                .filter(|d| d.vendor_id() == vid && d.product_id() == pid)
                .collect();
            let keys: Vec<(i32, u16, u16)> = entries
                .iter()
                .map(|d| (d.interface_number(), d.usage_page(), d.usage()))
                .collect();
            match pick_interface(&keys, interface) {
                Pick::Entry(at) => {
                    let path = entries[at].path().to_string_lossy().into_owned();
                    (entries[at].open_device(api)?, path)
                }
                Pick::AskDescriptors(at) => {
                    let candidates: Vec<&DeviceInfo> = at.iter().map(|&i| entries[i]).collect();
                    open_by_descriptor(api, &candidates)?
                }
                Pick::Nothing => {
                    bail!("no HID device for VID=0x{vid:04X} PID=0x{pid:04X} interface={interface}")
                }
            }
        };

        // Drain any buffered input reports so the first xfer sees a fresh reply.
        dev.set_blocking_mode(false).ok();
        let mut buf = [0u8; REPORT_SIZE];
        while let Ok(n) = dev.read_timeout(&mut buf, 0) {
            if n == 0 {
                break;
            }
        }
        Ok(IapDevice {
            link: Box::new(dev),
            path,
        })
    }

    /// An `IapDevice` over any [`ReportLink`], for a caller that brings its
    /// own pipe.
    pub fn with_link(link: Box<dyn ReportLink>, path: &str) -> IapDevice {
        IapDevice {
            link,
            path: path.to_string(),
        }
    }

    /// Write a report, wait, and read one response (or `None` on timeout).
    /// Ports `IAPDevice.xfer`.
    pub fn xfer(&self, packet: &[u8], timeout_ms: i32) -> Result<Response> {
        if packet.len() != REPORT_SIZE {
            bail!(
                "internal: report is {} bytes, must be {REPORT_SIZE}",
                packet.len()
            );
        }
        self.link.write_report(packet)?;
        std::thread::sleep(POST_WRITE_SLEEP);
        let mut buf = [0u8; REPORT_SIZE];
        let n = self.link.read_report(&mut buf, timeout_ms)?;
        if n == 0 {
            return Ok(None);
        }
        Ok(Some(buf[..n].to_vec()))
    }
}

// ---------------- Explicit-path identity check ----------------

/// What hidapi enumerates for one path.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PathIdentity {
    pub vid: u16,
    pub pid: u16,
    /// -1 where the platform does not number interfaces.
    pub interface: i32,
    pub usage_page: u16,
    pub usage: u16,
}

/// The identity hidapi enumerates for `path`, or `None` when the path is not
/// in the device list at all. Enumeration only: nothing is opened and nothing
/// is sent.
pub fn path_identity(api: &HidApi, path: &str) -> Option<PathIdentity> {
    api.device_list()
        .find(|d| d.path().to_string_lossy() == path)
        .map(|d| PathIdentity {
            vid: d.vendor_id(),
            pid: d.product_id(),
            interface: d.interface_number(),
            usage_page: d.usage_page(),
            usage: d.usage(),
        })
}

/// The comparison behind [`require_path_identity`], split out so it can be
/// tested without hardware.
///
/// Where the platform does not number the interface (-1), the number cannot
/// be compared, so the default IAP interface is accepted on VID:PID plus the
/// vendor usage instead. That admits interface 3 as well as 4 — they share
/// the usage — but 3 has no Output report, so a report sent to it goes
/// nowhere; it does keep the keyboard, mouse and consumer interfaces out.
pub fn check_path_identity(
    path: &str,
    found: Option<PathIdentity>,
    vid: u16,
    pid: u16,
    interface: i32,
) -> Result<()> {
    match found {
        None => bail!(
            "--hidraw {path} is not in the HID device list; refusing to send an IAP \
             report to a node that cannot be identified"
        ),
        Some(f) if (f.vid, f.pid, f.interface) == (vid, pid, interface) => Ok(()),
        Some(f)
            if f.interface < 0
                && interface == IAP_INTERFACE
                && (f.vid, f.pid) == (vid, pid)
                && (f.usage_page, f.usage) == (IAP_USAGE_PAGE, IAP_USAGE) =>
        {
            Ok(())
        }
        Some(f) => bail!(
            "--hidraw {path} is {:04X}:{:04X} if={}, not the {vid:04X}:{pid:04X} \
             if={interface} this command expects; refusing to send an IAP report to it \
             (pass matching --vid/--pid/--interface if that node really is the dongle)",
            f.vid,
            f.pid,
            f.interface
        ),
    }
}
/// `--hidraw` bypasses discovery: [`IapDevice::open`] opens whatever path it is
/// given with no check that the node carries the CLI's VID, PID or interface.
/// Actions that send a vendor report without first probing use this to
/// confirm, from hidapi enumeration alone (no IAP exchange), that the path is
/// the interface the selectors describe. Discovered devices never come here.
pub fn require_path_identity(
    api: &HidApi,
    path: &str,
    vid: u16,
    pid: u16,
    interface: i32,
) -> Result<()> {
    check_path_identity(path, path_identity(api, path), vid, pid, interface)
}

// ---------------- High-level IAP operations ----------------

/// `op_handshake`: see the call sites; part of the diagnostics readout.
pub fn op_handshake(dev: &IapDevice) -> Result<Response> {
    dev.xfer(
        &packet_simple(CMD_HANDSHAKE, HANDSHAKE_PAYLOAD),
        READ_TIMEOUT_MS,
    )
}

/// `op_arm`: see the call sites; part of the diagnostics readout.
pub fn op_arm(dev: &IapDevice) -> Result<Response> {
    dev.xfer(
        &packet_simple(CMD_GETDEVINFO, &[0x01, 0x00, 0x00, 0x00]),
        READ_TIMEOUT_MS,
    )
}

/// `op_disarm`: see the call sites; part of the diagnostics readout.
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

/// `op_version`: see the call sites; part of the diagnostics readout.
pub fn op_version(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_VERSION, &[]), READ_TIMEOUT_MS)
}

/// `op_bond_read`: see the call sites; part of the diagnostics readout.
pub fn op_bond_read(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_BOND_READ, &[]), READ_TIMEOUT_MS)
}

/// BENCH: time one X25519 scalar multiplication (KEX_BENCH firmware only).
pub fn op_kex_bench(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_simple(0x99, &[]), 5000)
}

/// Link-encryption counters (read-only, unarmed).
pub fn op_crypt_diag(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_CRYPT_DIAG, &[]), READ_TIMEOUT_MS)
}

/// Hand the dongle an entropy seed (unarmed; the firmware only ever mixes it
/// into its own pool). Reply `[0x9C][1][status]`.
pub fn op_kex_seed(dev: &IapDevice, seed: &[u8; KEX_SEED_BYTES]) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_KEX_SEED, seed), READ_TIMEOUT_MS)
}

/// BondClear (armed session only): firmware-side erase with readback.
pub fn op_bond_clear(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_BOND_CLEAR, &[]), READ_TIMEOUT_MS)
}

/// BondWrite (armed session only): the whole record, replacing the stored one.
pub fn op_bond_write(dev: &IapDevice, record: &[u8]) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_BOND_WRITE, record), READ_TIMEOUT_MS)
}

/// `op_status`: see the call sites; part of the diagnostics readout.
pub fn op_status(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_STATUS, &[]), READ_TIMEOUT_MS)
}

/// `op_fault`: see the call sites; part of the diagnostics readout.
pub fn op_fault(dev: &IapDevice) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_FAULT, &[]), READ_TIMEOUT_MS)
}

/// Read one RF diagnostics page (unarmed).
pub fn op_rf_diag(dev: &IapDevice, page: u8) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_RF_DIAG, &[page]), READ_TIMEOUT_MS)
}

/// RF intervention ladder rung (armed session): 1 re-arm, 2 re-init + re-arm.
pub fn op_rf_poke(dev: &IapDevice, rung: u8) -> Result<Response> {
    dev.xfer(&packet_simple(CMD_RF_POKE, &[rung]), READ_TIMEOUT_MS)
}

// ---------------- Armed sessions ----------------

/// What an armed exchange produced, and what became of the session it opened.
///
/// The two are separate because they fail separately: the exchange can
/// succeed and leave the session open, and a caller that reports only the
/// first would call a device clean that is still armed.
pub struct Armed<T> {
    pub result: Result<T>,
    /// `Err` when the closing disarm could not be confirmed. The session may
    /// then still be armed; it clears when the dongle is replugged.
    pub disarm: Result<()>,
}

impl<T> Armed<T> {
    /// Both, as one result: the exchange's own error first, then an
    /// unconfirmed disarm.
    pub fn strict(self) -> Result<T> {
        let value = self.result?;
        self.disarm?;
        Ok(value)
    }
}

/// How much of the disarm reply a command insists on.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum DisarmCheck {
    /// The reply must arrive and be one of the two acknowledgements.
    Acknowledged,
    /// Only a transport failure counts. After some commands the device has
    /// been seen to answer the disarm with a status frame instead.
    Sent,
}

/// ARM, run `body` with the arm reply, and disarm — on every path.
///
/// From the moment ARM is written the session may be armed, even when its
/// reply is lost or wrong, so the disarm is attempted whatever happened in
/// between, including when ARM itself appeared to fail.
pub fn armed<T>(
    dev: &IapDevice,
    check_disarm: DisarmCheck,
    body: impl FnOnce(&[u8]) -> Result<T>,
) -> Armed<T> {
    let result = op_arm(dev).and_then(|arm| {
        check("GetDevInfo(arm)", &arm, ACK_GETDEVINFO, None)?;
        body(arm.as_deref().unwrap_or_default())
    });
    let disarm = op_disarm(dev).and_then(|reply| {
        if check_disarm == DisarmCheck::Sent {
            return Ok(());
        }
        // Either ack means the write landed.
        let reply = reply.ok_or_else(|| anyhow!("GetDevInfo(disarm): no response (timeout)"))?;
        if reply.is_empty() || (reply[0] != ACK_OK && reply[0] != ACK_GETDEVINFO) {
            bail!(
                "GetDevInfo(disarm): unexpected ack; raw={}",
                hexsp(&reply, 8)
            );
        }
        Ok(())
    });
    Armed { result, disarm }
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
    // A device that answers short must produce an error, not a panic: these
    // bytes come off the wire and nothing guarantees their count.
    let need = if expected_status.is_some() { 3 } else { 1 };
    if r.len() < need {
        bail!(
            "{name}: short response, {} byte(s), need {need}; raw={}",
            r.len(),
            hexsp(r, 8)
        );
    }
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
                "{name}: status byte 0x{:02X} != 0x{:02X}; raw={}",
                r[2],
                st,
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

    // The seed packet is a cross-transport contract: after the HID report id
    // it is [9C][20][seed:32][sum], the 35 bytes the keyboard's MCU sends its
    // radio module over the UART (OpenController keyboard_uart.c, QMK
    // opencontroller_protocol.c).
    #[test]
    fn kex_seed_packet_is_the_uart_frame() {
        let mut seed = [0u8; KEX_SEED_BYTES];
        for (i, b) in seed.iter_mut().enumerate() {
            *b = (0x11 * i + 3) as u8;
        }
        let p = packet_simple(CMD_KEX_SEED, &seed);
        assert_eq!(p[0], 0x00);
        assert_eq!(&p[1..3], &[0x9C, 0x20]);
        assert_eq!(&p[3..35], &seed);
        let sum = p[1..35].iter().fold(0u8, |a, &b| a.wrapping_add(b));
        assert_eq!(p[35], sum);
        assert!(p[36..].iter().all(|&b| b == 0));
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

    fn identity(vid: u16, pid: u16, interface: i32) -> PathIdentity {
        PathIdentity {
            vid,
            pid,
            interface,
            usage_page: IAP_USAGE_PAGE,
            usage: IAP_USAGE,
        }
    }

    #[test]
    fn explicit_path_must_match_the_selectors() {
        let want = (DEFAULT_VID, DEFAULT_PID, IAP_INTERFACE);
        let found = Some(identity(want.0, want.1, want.2));
        assert!(check_path_identity("/dev/hidraw3", found, want.0, want.1, want.2).is_ok());

        // Not enumerated at all: nothing to compare against, so refuse.
        let err = check_path_identity("/dev/hidraw3", None, want.0, want.1, want.2)
            .unwrap_err()
            .to_string();
        assert!(err.contains("not in the HID device list"), "{err}");

        // Each selector on its own must be enough to refuse.
        for found in [
            identity(0x1209, DEFAULT_PID, IAP_INTERFACE),
            identity(DEFAULT_VID, 0x0001, IAP_INTERFACE),
            identity(DEFAULT_VID, DEFAULT_PID, 0),
        ] {
            let err = check_path_identity("/dev/hidraw3", Some(found), want.0, want.1, want.2)
                .unwrap_err()
                .to_string();
            assert!(err.contains("refusing"), "{err}");
            assert!(err.contains("0C45:FEFE if=4"), "{err}");
        }
    }

    #[test]
    fn an_unnumbered_path_is_accepted_on_the_vendor_usage_only() {
        let (vid, pid) = (DEFAULT_VID, DEFAULT_PID);
        let vendor = identity(vid, pid, -1);
        assert!(check_path_identity("p", Some(vendor), vid, pid, IAP_INTERFACE).is_ok());

        // The same dongle's keyboard interface must still be refused.
        let keyboard = PathIdentity {
            usage_page: 0x0001,
            usage: 0x0006,
            ..vendor
        };
        assert!(check_path_identity("p", Some(keyboard), vid, pid, IAP_INTERFACE).is_err());
        // The other vendor interface is not the IAP usage either.
        let other = PathIdentity {
            usage_page: 0xFFFF,
            usage: 0x0001,
            ..vendor
        };
        assert!(check_path_identity("p", Some(other), vid, pid, IAP_INTERFACE).is_err());
        // An explicit non-default interface cannot be confirmed without a number.
        assert!(check_path_identity("p", Some(vendor), vid, pid, 2).is_err());
        // And a different device entirely is refused whatever its usage.
        assert!(check_path_identity(
            "p",
            Some(identity(0x1209, pid, -1)),
            vid,
            pid,
            IAP_INTERFACE
        )
        .is_err());
    }

    /* --- choosing the interface ------------------------------------------ */

    /// The application's five interfaces as Linux and Windows report them.
    const NUMBERED: [(i32, u16, u16); 5] = [
        (0, 0x0001, 0x0006),
        (1, 0x0001, 0x0002),
        (2, 0x000C, 0x0001),
        (3, IAP_USAGE_PAGE, IAP_USAGE),
        (4, IAP_USAGE_PAGE, IAP_USAGE),
    ];

    fn unnumbered() -> Vec<(i32, u16, u16)> {
        NUMBERED.iter().map(|e| (-1, e.1, e.2)).collect()
    }

    #[test]
    fn an_interface_number_is_authoritative() {
        assert_eq!(pick_interface(&NUMBERED, 4), Pick::Entry(4));
        assert_eq!(pick_interface(&NUMBERED, 3), Pick::Entry(3));
        // Not present: no fallback while the platform does number interfaces,
        // or a missing interface 4 would quietly become interface 3.
        assert_eq!(pick_interface(&NUMBERED[..4], 4), Pick::Nothing);
        assert_eq!(pick_interface(&[], 4), Pick::Nothing);
    }

    #[test]
    fn unnumbered_interfaces_fall_back_to_the_vendor_usage() {
        // Both vendor interfaces declare the usage, so both are candidates:
        // the usage narrows, the report descriptor decides.
        assert_eq!(
            pick_interface(&unnumbered(), IAP_INTERFACE),
            Pick::AskDescriptors(vec![3, 4])
        );
        // An explicit other interface cannot be honoured without numbers.
        assert_eq!(pick_interface(&unnumbered(), 2), Pick::Nothing);
        // Only the bootloader is on the bus: nothing declares the usage.
        assert_eq!(
            pick_interface(&[(-1, 0xFF00, 0x0001)], IAP_INTERFACE),
            Pick::Nothing
        );
    }

    /// The report descriptors of interfaces 3 and 4, copied from
    /// firmware/common/src/usb_descriptors.c. Same usage, and only interface
    /// 4 has an Output report — the fact the fallback rests on.
    const DESC_IF3: [u8; 21] = [
        0x06, 0x60, 0xFF, 0x09, 0x61, 0xA1, 0x01, 0x09, 0x62, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x95,
        0x20, 0x75, 0x08, 0x81, 0x02, 0xC0,
    ];
    const DESC_IF4: [u8; 34] = [
        0x06, 0x60, 0xFF, 0x09, 0x61, 0xA1, 0x01, 0x09, 0x62, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x95,
        0x40, 0x75, 0x08, 0x81, 0x02, 0x09, 0x63, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x95, 0x40, 0x75,
        0x08, 0x91, 0x02, 0xC0,
    ];

    #[test]
    fn only_the_iap_interface_has_an_output_report() {
        assert!(!has_output_report(&DESC_IF3));
        assert!(has_output_report(&DESC_IF4));
    }

    #[test]
    fn output_detection_parses_items_and_is_not_fooled_by_data() {
        // 0x91 as the DATA byte of a Report Count item is not an Output item.
        assert!(!has_output_report(&[0x95, 0x91, 0x81, 0x02]));
        // Every size form of the Output item counts.
        assert!(has_output_report(&[0x90]));
        assert!(has_output_report(&[0x92, 0x02, 0x01]));
        // A long item is skipped whole, whatever it contains.
        assert!(!has_output_report(&[0xFE, 0x02, 0x10, 0x91, 0x02, 0xC0]));
        // Truncated input ends the walk; it must not panic.
        assert!(!has_output_report(&[0x26, 0xFF]));
        assert!(!has_output_report(&[0xFE]));
        assert!(!has_output_report(&[]));
    }

    /* --- armed sessions --------------------------------------------------- */

    use crate::scripted::ScriptedLink;

    fn session(arm: Option<Vec<u8>>, disarm: Option<Vec<u8>>) -> ScriptedLink {
        ScriptedLink::new(move |cmd, body| match (cmd, body.first()) {
            (CMD_GETDEVINFO, Some(1)) => arm.clone(),
            (CMD_GETDEVINFO, _) => disarm.clone(),
            _ => Some(vec![0xEE]),
        })
    }

    fn ok_arm() -> Option<Vec<u8>> {
        Some(vec![ACK_GETDEVINFO])
    }

    fn ok_disarm() -> Option<Vec<u8>> {
        Some(vec![ACK_OK])
    }

    /// GetDevInfo bodies as sent: 1 arms, 0 disarms.
    fn session_bytes(link: &ScriptedLink) -> Vec<u8> {
        link.log()
            .iter()
            .filter(|(cmd, _)| *cmd == CMD_GETDEVINFO)
            .map(|(_, body)| body[0])
            .collect()
    }

    #[test]
    fn an_armed_body_runs_between_arm_and_disarm() {
        let link = session(ok_arm(), ok_disarm());
        let dev = link.device();
        let outcome = armed(&dev, DisarmCheck::Acknowledged, |arm| {
            assert_eq!(arm[0], ACK_GETDEVINFO, "the body is handed the arm reply");
            dev.xfer(&packet_simple(CMD_BOND_READ, &[]), 10)?;
            Ok(7)
        });
        assert_eq!(outcome.strict().unwrap(), 7);
        assert_eq!(
            link.commands(),
            vec![CMD_GETDEVINFO, CMD_BOND_READ, CMD_GETDEVINFO]
        );
        assert_eq!(session_bytes(&link), vec![1, 0]);
    }

    #[test]
    fn a_failing_body_is_still_followed_by_the_disarm() {
        let link = session(ok_arm(), ok_disarm());
        let outcome: Armed<()> = armed(&link.device(), DisarmCheck::Acknowledged, |_| {
            bail!("the exchange failed")
        });
        assert!(outcome.result.is_err());
        assert!(outcome.disarm.is_ok());
        assert_eq!(session_bytes(&link), vec![1, 0]);
    }

    /// ARM was written, so the session may be armed whatever came back —
    /// nothing, or the wrong thing. The body must not run, and the disarm
    /// must still go out.
    #[test]
    fn an_arm_that_is_lost_or_wrong_skips_the_body_and_still_disarms() {
        for arm in [None, Some(vec![0x55])] {
            let link = session(arm, ok_disarm());
            let mut ran = false;
            let outcome = armed(&link.device(), DisarmCheck::Acknowledged, |_| {
                ran = true;
                Ok(())
            });
            assert!(outcome.result.is_err());
            assert!(!ran, "the body must not run without a confirmed arm");
            assert_eq!(session_bytes(&link), vec![1, 0]);
        }
    }

    #[test]
    fn an_acknowledged_disarm_accepts_either_ack_and_nothing_else() {
        for (disarm, confirmed) in [
            (Some(vec![ACK_OK]), true),
            (Some(vec![ACK_GETDEVINFO]), true),
            (Some(vec![0x77]), false),
            (None, false),
        ] {
            let link = session(ok_arm(), disarm);
            let outcome = armed(&link.device(), DisarmCheck::Acknowledged, |_| Ok(()));
            assert!(outcome.result.is_ok());
            assert_eq!(outcome.disarm.is_ok(), confirmed);
        }
    }

    /// After a bond clear the device has been seen to answer the disarm with
    /// a status frame, so those commands only require that it was sent.
    #[test]
    fn a_sent_disarm_does_not_judge_the_reply() {
        for disarm in [Some(vec![0x77]), None] {
            let link = session(ok_arm(), disarm);
            let outcome = armed(&link.device(), DisarmCheck::Sent, |_| Ok(()));
            assert!(outcome.disarm.is_ok());
            assert_eq!(session_bytes(&link), vec![1, 0]);
        }
    }
}
