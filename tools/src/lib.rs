// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! Host library for OpenDongle dongles (WCH CH570 / CH592F): reading device,
//! firmware, bond, fault and RF state over the dongle's vendor HID interface,
//! and handing the device to the OpenBoot bootloader for updates. The
//! `opendongle` CLI is a thin front end over this crate.
//!
//! - [`iap`] — the vendor channel: packets, the device handle, and armed
//!   sessions ([`iap::armed`]), which always disarm.
//! - [`status`], [`bond`], [`fault`], [`rfdiag`], [`crypt`] — one readout
//!   each. They return what they read; nothing in them prints.
//! - [`flows`] — identity (`probe`) and the reboot request.
//! - [`handoff`] — the reboot into OpenBoot, and telling from the bus whether
//!   the dongle that was addressed is the one that arrived.
//! - [`hex`] — update images and the family guard.
//! - [`console`] — the CLI's rendering of all of the above as text.
//!
//! It does not flash firmware. Updates happen in the bootloader over OBP,
//! driven by the `openboot` crate.
//!
//! Build without default features to leave out the CLI's argument parser.

pub mod bond;
pub mod console;
pub mod crypt;
pub mod fault;
pub mod flows;
pub mod handoff;
pub mod hex;
pub mod iap;
pub mod rfdiag;
pub mod scripted;
pub mod status;
