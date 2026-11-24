// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! High-level flows. Since the OpenBoot cutover the vendor IAP interface no
//! longer flashes: these read device identity, and updates happen in the
//! OpenBoot bootloader over OBP (`opendongle --enter-bootloader`, then the
//! `openboot` CLI).

use anyhow::{anyhow, bail, Result};

use crate::iap::*;

/// What GetDevInfo reported.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DevInfo {
    /// Formerly the staging base; the firmware reports 0 since the OpenBoot
    /// cutover (field retained so the response shape never moved).
    pub reserved_geometry: u32,
    /// Formerly the program block size; 0 since the OpenBoot cutover.
    pub reserved_block: u16,
    pub chip_id: u16,
    pub chip_extra: u16,
    /// The reply as received, for display.
    pub raw: Vec<u8>,
}

impl DevInfo {
    /// The chip family byte: the low byte of `chip_id`, and what an update
    /// image's ODG2 family is checked against.
    pub fn family(&self) -> u8 {
        (self.chip_id & 0xFF) as u8
    }

    pub fn family_name(&self) -> &'static str {
        match self.family() {
            0x92 => "CH592 family",
            0x82 => "CH582 family",
            0x70 => "CH570 family",
            _ => "unexpected",
        }
    }

    /// Firmware from before the OpenBoot cutover still advertises its staging
    /// geometry in the fields that are now reserved.
    pub fn is_pre_openboot(&self) -> bool {
        self.reserved_geometry != 0 || self.reserved_block != 0
    }
}

/// The opening handshake. Returns the reply, whose ack has been checked.
pub fn handshake(dev: &IapDevice) -> Result<Vec<u8>> {
    let r = op_handshake(dev)?;
    check("Handshake", &r, ACK_HANDSHAKE, None)?;
    Ok(r.unwrap())
}

/// Arm, read GetDevInfo, disarm. The disarm is attempted on every path and
/// must be acknowledged.
pub fn read_devinfo(dev: &IapDevice) -> Armed<DevInfo> {
    armed(dev, DisarmCheck::Acknowledged, |rr| {
        // check() only guarantees the ack byte. The decode below reads through
        // rr[11], and these bytes came off the wire from a device that may be
        // wedged, mid-reset, or not the thing we think it is - so a short but
        // ack-correct reply must be an error here, not an out-of-bounds panic.
        if rr.len() < DEVINFO_LEN {
            bail!(
                "GetDevInfo(arm): short response, {} byte(s), need {DEVINFO_LEN}; raw={}",
                rr.len(),
                hexsp(rr, 8)
            );
        }
        Ok(DevInfo {
            reserved_geometry: u32::from_le_bytes([rr[2], rr[3], rr[4], rr[5]]),
            reserved_block: u16::from_le_bytes([rr[6], rr[7]]),
            chip_id: u16::from_le_bytes([rr[8], rr[9]]),
            chip_extra: u16::from_le_bytes([rr[10], rr[11]]),
            raw: rr.to_vec(),
        })
    })
}

/// Handshake + GetDevInfo read, leaving the session disarmed. Fails if either
/// step fails or the disarm is not acknowledged.
pub fn probe(dev: &IapDevice) -> Result<DevInfo> {
    handshake(dev)?;
    read_devinfo(dev).strict()
}

/// The firmware's version string (command 0x90, unarmed).
pub fn firmware_version(dev: &IapDevice) -> Result<String> {
    let response = op_version(dev)?.ok_or_else(|| anyhow!("Version: no response (timeout)"))?;
    let end = response
        .iter()
        .position(|&byte| byte == 0)
        .unwrap_or(response.len());
    Ok(String::from_utf8_lossy(&response[..end]).into_owned())
}

/// Armed enter-bootloader sequence: handshake, arm, 0x85 with the magic
/// payload, expecting the status-0 reply before the device resets into
/// OpenBoot. The caller watches for the device leaving the bus.
///
/// There is no disarm on success — the device is resetting, and the reset
/// clears the session. A device that REFUSES is still running with the
/// session armed, so that path disarms before reporting the refusal. A reply
/// that never comes is neither: the device may be mid-reset, and the caller
/// has to look at the bus to find out.
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
    if rr.len() < 4 || rr[0] != ACK_OK || rr[1] != 0x01 || rr[2] != 0 || rr[3] != 0 {
        let _ = op_disarm(dev);
        bail!(
            "EnterBootloader: refused; ack=0x{:02X} status=0x{:02X} raw={}",
            rr.first().copied().unwrap_or(0xFF),
            rr.get(2).copied().unwrap_or(0xFF),
            hexsp(rr, 8)
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scripted::ScriptedLink;

    fn answering(cmd: u8, body: &[u8]) -> Option<Vec<u8>> {
        Some(match cmd {
            0x5A => vec![0xA5],
            0x84 if body[0] == 1 => vec![0x04, 0x0A, 0, 0, 0, 0, 0, 0, 0x92, 0, 0, 0],
            0x84 => vec![0x0F, 0x01, 0, 0],
            0x85 => vec![0x0F, 0x01, 0, 0],
            _ => return None,
        })
    }

    /// The (arm?, disarm?) pair as sent: GetDevInfo bodies start 1 or 0.
    fn session_bytes(link: &ScriptedLink) -> Vec<u8> {
        link.log()
            .iter()
            .filter(|(cmd, _)| *cmd == 0x84)
            .map(|(_, body)| body[0])
            .collect()
    }

    #[test]
    fn probe_reads_the_family_and_leaves_the_session_disarmed() {
        let link = ScriptedLink::new(answering);
        let info = probe(&link.device()).unwrap();
        assert_eq!(info.family(), 0x92);
        assert_eq!(info.family_name(), "CH592 family");
        assert!(!info.is_pre_openboot());
        assert_eq!(link.commands(), vec![0x5A, 0x84, 0x84]);
        assert_eq!(session_bytes(&link), vec![1, 0]);
    }

    /// The bug this guards: a failure between ARM and the disarm used to
    /// return early and leave the session armed.
    #[test]
    fn a_wrong_arm_reply_still_disarms() {
        let link = ScriptedLink::new(|cmd, body| match cmd {
            0x84 if body[0] == 1 => Some(vec![0x55]),
            _ => answering(cmd, body),
        });
        let err = probe(&link.device()).unwrap_err().to_string();
        assert!(err.contains("bad ack"), "{err}");
        assert_eq!(session_bytes(&link), vec![1, 0], "disarm must follow");
    }

    #[test]
    fn a_lost_arm_reply_still_disarms() {
        // ARM was written; whether it landed is unknown, so it is treated as
        // having landed.
        let link = ScriptedLink::new(|cmd, body| match cmd {
            0x84 if body[0] == 1 => None,
            _ => answering(cmd, body),
        });
        let outcome = read_devinfo(&link.device());
        assert!(outcome.result.is_err());
        assert!(outcome.disarm.is_ok());
        assert_eq!(session_bytes(&link), vec![1, 0]);
    }

    #[test]
    fn an_unacknowledged_disarm_is_reported_separately_from_the_read() {
        let link = ScriptedLink::new(|cmd, body| match cmd {
            0x84 if body[0] == 0 => None,
            _ => answering(cmd, body),
        });
        let outcome = read_devinfo(&link.device());
        assert_eq!(outcome.result.as_ref().unwrap().family(), 0x92);
        let err = outcome.disarm.as_ref().unwrap_err().to_string();
        assert!(err.contains("GetDevInfo(disarm)"), "{err}");
        // And `strict` turns that into a failure of the whole read.
        assert!(outcome.strict().is_err());
    }

    #[test]
    fn enter_bootloader_does_not_disarm_a_device_that_is_resetting() {
        let link = ScriptedLink::new(answering);
        enter_bootloader(&link.device()).unwrap();
        assert_eq!(link.commands(), vec![0x5A, 0x84, 0x85]);
    }

    #[test]
    fn a_refused_enter_bootloader_disarms_the_session_it_opened() {
        let link = ScriptedLink::new(|cmd, body| match cmd {
            0x85 => Some(vec![0x0F, 0x01, 0xE2, 0x00]),
            _ => answering(cmd, body),
        });
        let err = enter_bootloader(&link.device()).unwrap_err().to_string();
        assert!(err.contains("refused"), "{err}");
        assert!(err.contains("status=0xE2"), "{err}");
        assert_eq!(link.commands(), vec![0x5A, 0x84, 0x85, 0x84]);
        assert_eq!(session_bytes(&link), vec![1, 0]);
    }

    #[test]
    fn a_silent_enter_bootloader_is_left_for_the_bus_to_settle() {
        let link = ScriptedLink::new(|cmd, body| match cmd {
            0x85 => None,
            _ => answering(cmd, body),
        });
        let err = enter_bootloader(&link.device()).unwrap_err().to_string();
        assert!(err.contains("no response"), "{err}");
        assert_eq!(link.commands(), vec![0x5A, 0x84, 0x85]);
    }

    #[test]
    fn the_version_string_stops_at_its_terminator() {
        let link = ScriptedLink::new(|cmd, _| (cmd == 0x90).then(|| b"0.97.1\0junk".to_vec()));
        assert_eq!(firmware_version(&link.device()).unwrap(), "0.97.1");
    }
}
