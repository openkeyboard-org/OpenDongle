// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! One-step firmware update for OpenDongle dongles.
//!
//! [`artifact::Artifact`] is an update bundle that has passed every check a
//! file can be given; [`transaction::run`] takes a dongle running its
//! application through the reboot into OpenBoot, the flash, the boot, and a
//! confirmation of what came back. Both are plain library calls with their
//! devices, bus and clock passed in, so a front end only has to draw the
//! result — and the whole thing is tested against a simulated dongle.
//!
//! This crate is where the two host libraries meet: `opendongle` speaks to
//! the application and `openboot` to the bootloader, and neither knows the
//! other exists.

pub mod artifact;
pub mod transaction;

#[cfg(test)]
mod sim;
#[cfg(test)]
mod tests;

use anyhow::Result;
use hidapi::HidApi;

use openboot::transport::hid::HidTransport;
use opendongle::handoff;
use opendongle::iap::IapDevice;

use transaction::{Bootloader, Devices};

/// The real devices, through hidapi.
pub struct HidDevices;

impl Devices for HidDevices {
    fn open_bootloader(&mut self, vid: u16, pid: u16, path: &str) -> Result<Bootloader> {
        // The serial comes from enumeration, before the interface is opened:
        // it is the claim the HELLO that follows is checked against.
        let serial = HidTransport::list(vid, pid)?
            .into_iter()
            .find(|candidate| candidate.path == path)
            .and_then(|candidate| candidate.serial)
            .filter(|serial| !serial.is_empty());
        let transport = HidTransport::open_path(vid, pid, path)?;
        Ok(Bootloader {
            transport: Box::new(transport),
            serial,
        })
    }

    fn open_applications(&mut self, vid: u16, pid: u16, interface: i32) -> Vec<IapDevice> {
        let Ok(api) = HidApi::new() else {
            return Vec::new();
        };
        let paths = handoff::application_paths(&handoff::entries(&api), vid, pid, interface);
        if paths.is_empty() {
            // No interface carries the IAP interface's number. Either no
            // application is on the bus, or this platform does not number
            // interfaces; discovery copes with the second and finds nothing
            // in the first.
            return IapDevice::open(&api, vid, pid, interface, None)
                .into_iter()
                .collect();
        }
        paths
            .iter()
            .filter_map(|path| IapDevice::open(&api, vid, pid, interface, Some(path)).ok())
            .collect()
    }
}
