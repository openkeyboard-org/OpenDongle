// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! `opendongle-update <bundle.obb>` — update a dongle in one step.
//!
//! Checks the bundle, reboots the dongle into OpenBoot, flashes the build for
//! whichever slot it offers, boots it, and confirms that the dongle that was
//! addressed came back running the build that was sent.
//!
//! Exit codes: 0 the dongle is running the new build; 2 the reboot into the
//! bootloader could not be pinned to one dongle (nothing was written); 1
//! everything else, including an update that was committed but could not be
//! confirmed.

use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{Context, Result};
use clap::Parser;
use hidapi::HidApi;

use openboot::console::{Console as BootConsole, TextObserver};
use openboot::flows::{Observer as _, Verification};
use opendongle::console::{report_handoff, Console as DongleConsole, HandoffContext};
use opendongle::handoff::{self, HidBus, SystemClock, Target};
use opendongle::iap::{
    path_identity, require_path_identity, IapDevice, DEFAULT_PID, DEFAULT_VID, IAP_INTERFACE,
};
use opendongle_update::artifact::Artifact;
use opendongle_update::transaction::{
    self, Confirmation, Identity, Observer, Outcome, Progress, Request, Stage, Stopped,
    RETURN_DEADLINE,
};
use opendongle_update::HidDevices;

fn parse_u16_auto(s: &str) -> Result<u16, String> {
    let t = s.trim();
    let parsed = match t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        Some(hex) => u16::from_str_radix(hex, 16),
        None => t.parse::<u16>(),
    };
    parsed.map_err(|e| format!("invalid value '{s}': {e}"))
}

#[derive(Parser)]
#[command(
    name = "opendongle-update",
    version,
    about = "Update an OpenDongle in one step: check the bundle, reboot into \
             OpenBoot, flash, boot, and confirm what came back."
)]
struct Cli {
    /// The update bundle (.obb) for this dongle's chip
    bundle: PathBuf,

    /// Only check the bundle; touch no device
    #[arg(long)]
    check: bool,

    /// USB VID (default 0x0C45)
    #[arg(long, value_parser = parse_u16_auto, default_value_t = DEFAULT_VID)]
    vid: u16,

    /// USB PID (default 0xFEFE)
    #[arg(long, value_parser = parse_u16_auto, default_value_t = DEFAULT_PID)]
    pid: u16,

    /// USB interface number of the application's vendor channel (default 4)
    #[arg(long, default_value_t = IAP_INTERFACE)]
    interface: i32,

    /// Explicit HID device path of the dongle to update (skip discovery)
    #[arg(long, visible_alias = "path")]
    hidraw: Option<String>,
}

/// Prints the stages as they happen, and the flash as `openboot` prints it.
struct Printer<'c> {
    flash: TextObserver<'c>,
}

impl Observer for Printer<'_> {
    fn progress(&mut self, progress: Progress<'_>) {
        match progress {
            Progress::Stage(Stage::Rebooting) => println!("rebooting into the bootloader..."),
            Progress::RebootAccepted => {}
            Progress::Stage(Stage::Opening) => println!("bootloader arrived; opening it"),
            Progress::Stage(Stage::Flashing) => {}
            Progress::Stage(Stage::Confirming) => {
                println!("waiting for the dongle to come back...")
            }
            Progress::Flash(event) => self.flash.event(event),
        }
    }
}

fn describe(id: &Identity) -> String {
    let uid: String = id.uid.iter().map(|b| format!("{b:02X}")).collect();
    format!("UID {uid}, build {:08X}", id.build_id)
}

/// Say what was found running, relative to what was hoped for.
fn report_running(c: &Confirmation, hoped: &str, cli: &Cli) {
    match c {
        Confirmation::Confirmed(id) => println!("the dongle is running {hoped} ({})", describe(id)),
        Confirmation::WrongBuild(id) => eprintln!(
            "the dongle came back, but NOT running {hoped}: it reports {}",
            describe(id)
        ),
        Confirmation::DifferentUnit(id) => eprintln!(
            "a dongle answered, but it is not the one that was addressed ({}); \
             the one that was has not come back",
            describe(id)
        ),
        Confirmation::NoApplication => {
            eprintln!(
                "no dongle application answered within {} s.",
                RETURN_DEADLINE.as_secs()
            );
            eprintln!(
                "       Replug the dongle. If it stays in the bootloader, \
                 `openboot --vid 0x{:04X} --pid 0x{:04X} boot` starts whatever it can.",
                cli.vid, cli.pid
            );
        }
    }
}

fn run(cli: &Cli) -> Result<ExitCode> {
    // The file first: nothing below is worth doing for a bundle that would be
    // refused, and this part needs no device.
    let artifact = Artifact::load(&cli.bundle)?;
    println!(
        "bundle {}: OpenDongle {} update",
        cli.bundle.display(),
        artifact.chip_name()
    );
    for (slot, v) in ["A", "B"].iter().zip(&artifact.variants) {
        println!(
            "  slot {slot}  base 0x{:08X}  {:>7} B  build {:08X}",
            v.base, v.len, v.build_id
        );
    }
    if cli.check {
        return Ok(ExitCode::SUCCESS);
    }

    let api = HidApi::new()?;
    if let Some(path) = cli.hidraw.as_deref() {
        require_path_identity(&api, path, cli.vid, cli.pid, cli.interface)?;
    }
    let dev = IapDevice::open(&api, cli.vid, cli.pid, cli.interface, cli.hidraw.as_deref())
        .context(
            "no dongle running its application was found. If it is already in the \
             bootloader, flash it with `openboot flash --force <bundle>`",
        )?;

    // Which dongle. Discovery opens the first match, so with several running
    // their application it would be an arbitrary one: refuse.
    let target = match cli.hidraw.as_deref() {
        None => {
            let apps = handoff::application_paths(
                &handoff::entries(&HidApi::new()?),
                cli.vid,
                cli.pid,
                cli.interface,
            );
            if apps.len() > 1 {
                eprintln!(
                    "ERROR: {} dongles are running their application on {:04X}:{:04X}; \
                     cannot say which one to update.",
                    apps.len(),
                    cli.vid,
                    cli.pid
                );
                eprintln!("       Unplug the others, or name this one with --hidraw.");
                return Ok(ExitCode::from(2));
            }
            Target {
                path: dev.path.clone(),
                vid: cli.vid,
                pid: cli.pid,
            }
        }
        Some(path) => {
            let id = path_identity(&HidApi::new()?, path)
                .context("the --hidraw path is not in the HID device list")?;
            Target {
                path: path.to_string(),
                vid: id.vid,
                pid: id.pid,
            }
        }
    };

    let before = Identity::read(&dev)?;
    println!("dongle {}: {}", dev.path, describe(&before));
    if artifact
        .variants
        .iter()
        .any(|v| v.build_id == before.build_id)
    {
        println!("note: the dongle is already running a build from this bundle; flashing it again");
    }

    let request = Request {
        target,
        interface: cli.interface,
        artifact: &artifact,
    };
    let mut flash_console = BootConsole::stdio();
    let mut printer = Printer {
        flash: TextObserver::new(&mut flash_console),
    };
    let outcome = transaction::run(
        dev,
        &request,
        &mut HidBus,
        &mut SystemClock,
        &mut HidDevices,
        &mut printer,
    )?;

    Ok(match outcome {
        Outcome::Refused(why) => {
            eprintln!("ERROR: {why}");
            eprintln!("       Nothing was sent; the dongle is running as before.");
            ExitCode::from(1)
        }
        Outcome::HandoffFailed(handoff) => {
            let cx = HandoffContext {
                vid: cli.vid,
                pid: cli.pid,
                interface: cli.interface,
                bundle: Some(&cli.bundle),
            };
            ExitCode::from(report_handoff(&handoff, &cx, &mut DongleConsole::stdio()))
        }
        Outcome::NotCommitted { why, back } => {
            match why {
                Stopped::Cancelled => eprintln!("update cancelled before anything was committed."),
                Stopped::Failed(e) => eprintln!("ERROR: the update was not applied: {e}"),
            }
            report_running(&back, "its previous firmware", cli);
            ExitCode::from(1)
        }
        Outcome::Committed {
            verification,
            problem,
            running,
        } => {
            let confirmed = matches!(running, Confirmation::Confirmed(_));
            match verification {
                Some(Verification::LiveCrc { .. }) => {}
                Some(Verification::CommitOnly) => println!(
                    "note: this chip cannot cross-check flash it has just written; \
                     the bootloader's own commit check is what vouches for the image"
                ),
                Some(Verification::Skipped) | None => {}
            }
            if let Some(problem) = &problem {
                // Worth a line either way: it explains an unconfirmed result,
                // and a confirmed one is still an update that did not go
                // cleanly.
                eprintln!("note: {problem}");
            }
            report_running(&running, "the new firmware", cli);
            if confirmed {
                println!("update complete: build {:08X} -> {}", before.build_id, {
                    match running {
                        Confirmation::Confirmed(id) => format!("{:08X}", id.build_id),
                        _ => unreachable!(),
                    }
                });
                ExitCode::SUCCESS
            } else {
                eprintln!("ERROR: the image was committed, but the update could not be confirmed.");
                ExitCode::from(1)
            }
        }
    })
}

fn main() -> ExitCode {
    match run(&Cli::parse()) {
        Ok(code) => code,
        Err(e) => {
            eprintln!("ERROR: {e:#}");
            ExitCode::from(1)
        }
    }
}
