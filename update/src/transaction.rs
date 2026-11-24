// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! One firmware update, start to finish, as a single transaction.
//!
//! Doing this by hand is two tools and a race: `opendongle --enter-bootloader`,
//! then `openboot flash` within the bootloader's ten second idle window. This
//! does both, and adds what neither tool can do alone: it knows what the
//! dongle was running before, what it wrote, and therefore whether what came
//! back afterwards is the dongle it addressed running the build it sent.
//!
//! ## What it guarantees, and what it cannot
//!
//! - **It always sends the device back.** A successful HELLO stops the
//!   bootloader's idle auto-boot until a reset, so a host that stops there
//!   leaves the dongle in the bootloader for good. Every path out of this
//!   module that could have sent a HELLO ends with a BOOT.
//! - **It never commits a cancelled update, and never abandons a committed
//!   one.** Cancellation is honoured up to COMMIT and not after.
//! - **A lost reply is resolved, not guessed.** COMMIT's is settled by asking
//!   the device which slot is now active; BOOT's by watching what comes back.
//!   BOOT itself is never sent twice in one session.
//!
//! It cannot prove, before it erases, that the bootloader it opened is the
//! dongle it rebooted. The application reports the ROM MAC plus a checksum;
//! the bootloader reports the flash unique ID on CH592 and the bare MAC on
//! CH570. With one dongle attached there is nothing else it could be, and the
//! hand-off refuses when there is more than one. What this module adds is the
//! check AFTERWARDS: the UID the application reports must be the one it
//! reported before. A write that landed on some other unit shows up as the
//! addressed dongle coming back with its old build.

use std::time::Duration;

use anyhow::{Context, Result};

use openboot::bundle::Source;
use openboot::client::{BootClient, BootMode, ClientError};
use openboot::flows::{self, Cancelled, Event, FlashOpts, Verification, VerifyMismatch};
use openboot::transport::Transport;
use opendongle::handoff::{self, Bus, Clock, Handoff, Target, POLL};
use opendongle::iap::IapDevice;
use opendongle::status::read_status;

use crate::artifact::Artifact;

/// How long the application has to come back, from the moment the bootloader
/// was let go. Covers the reset, the boot-time image CRC and enumeration — and,
/// where the bootloader was never opened, its own ten second idle timeout.
pub const RETURN_DEADLINE: Duration = Duration::from_secs(20);
/// If nothing has come back by now and a bootloader is still on the bus, it
/// is asked once more to boot.
const RESCUE_AFTER: Duration = Duration::from_secs(4);

/* --- what the caller supplies ------------------------------------------- */

/// What a dongle's application says about itself (status 0x91, unarmed).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Identity {
    /// The ROM MAC and its checksum. Stable for a unit, and the same before
    /// and after an update — which is what makes it worth comparing.
    pub uid: [u8; 8],
    /// The ODG2 family byte.
    pub family: u8,
    pub build_id: u32,
    pub image_len: u32,
}

impl Identity {
    pub fn read(dev: &IapDevice) -> Result<Identity> {
        let status = read_status(dev)?;
        Ok(Identity {
            uid: status.uid,
            family: status.family,
            build_id: status.build_id,
            image_len: status.image_len,
        })
    }
}

/// An open bootloader interface.
pub struct Bootloader {
    pub transport: Box<dyn Transport>,
    /// The USB serial string enumeration reported for it, where the platform
    /// gives one. OpenBoot builds it from the UID it returns in HELLO, so the
    /// two must agree.
    pub serial: Option<String>,
}

/// Opening devices, as a seam: the real one talks to hidapi, tests supply a
/// simulated dongle.
pub trait Devices {
    /// Open the OpenBoot interface at exactly `path`.
    fn open_bootloader(&mut self, vid: u16, pid: u16, path: &str) -> Result<Bootloader>;
    /// Open the IAP interface of every dongle that is running its application
    /// right now. That can be more than the one being updated — another
    /// dongle may be attached — so the caller tells them apart by what each
    /// reports. One that cannot be opened is left out.
    fn open_applications(&mut self, vid: u16, pid: u16, interface: i32) -> Vec<IapDevice>;
}

/// Where the transaction has got to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Stage {
    /// Asking the application to reboot, and watching the bus for it.
    Rebooting,
    /// Opening the bootloader that arrived and checking it.
    Opening,
    /// Erasing, writing, committing. The flash events follow.
    Flashing,
    /// Waiting for an application to come back.
    Confirming,
}

/// One step of an update, reported as it happens.
#[derive(Debug, Clone, Copy)]
pub enum Progress<'a> {
    Stage(Stage),
    /// The dongle agreed to reboot.
    RebootAccepted,
    /// What OpenBoot's flash is doing.
    Flash(Event<'a>),
}

/// Receives progress, and may ask for the update to stop.
pub trait Observer {
    fn progress(&mut self, progress: Progress<'_>);

    /// Asked between requests while erasing and writing. Returning true stops
    /// the update before anything is committed and sends the dongle back to
    /// the application it was running. It is not asked from COMMIT onwards.
    fn cancelled(&mut self) -> bool {
        false
    }
}

/// Observes nothing and never cancels.
impl Observer for () {
    fn progress(&mut self, _progress: Progress<'_>) {}
}

/// What to update, and with what.
pub struct Request<'a> {
    /// The application interface that was opened.
    pub target: Target,
    /// The IAP interface number, for finding the application again afterwards.
    pub interface: i32,
    pub artifact: &'a Artifact,
}

/* --- how it ended ------------------------------------------------------- */

/// What was running once the dust settled.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Confirmation {
    /// The dongle that was addressed, running the build that was expected.
    Confirmed(Identity),
    /// The dongle that was addressed, running something else.
    WrongBuild(Identity),
    /// A dongle answered, but not the one that was addressed.
    DifferentUnit(Identity),
    /// No application answered within [`RETURN_DEADLINE`].
    NoApplication,
}

/// Why an update stopped short of COMMIT.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Stopped {
    Cancelled,
    Failed(String),
}

/// How an update ended.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Outcome {
    /// Refused before anything that changes the dongle was sent.
    Refused(String),
    /// The reboot into the bootloader could not be pinned to this dongle.
    /// Nothing was written.
    HandoffFailed(Handoff),
    /// The update stopped before COMMIT, so the previous application is still
    /// the one the dongle boots. `back` is whether it was seen to.
    NotCommitted { why: Stopped, back: Confirmation },
    /// The new image was committed. `running` is whether the dongle was then
    /// seen running it; `problem` is anything that went wrong on the way
    /// which a confirmed result makes moot and an unconfirmed one explains.
    Committed {
        /// `None` when the check could not be completed.
        verification: Option<Verification>,
        problem: Option<String>,
        running: Confirmation,
    },
    /// COMMIT was sent, its reply never came, the device could not be asked
    /// what had happened, and what came back afterwards does not settle it.
    /// The new image may or may not be what the dongle boots next. `back` is
    /// what was seen; it is never the addressed dongle on a known build,
    /// since that would have settled it.
    Unknown { why: String, back: Confirmation },
}

impl Outcome {
    /// The addressed dongle is running the build that was sent.
    pub fn succeeded(&self) -> bool {
        matches!(
            self,
            Outcome::Committed {
                running: Confirmation::Confirmed(_),
                ..
            }
        )
    }
}

/* --- the flash, and what became of it ----------------------------------- */

/// How far OpenBoot's flash got, read off the events it reported.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Reached {
    Nothing,
    /// Every WRITE was acknowledged; COMMIT is next.
    Written,
    /// COMMIT was acknowledged.
    Committed,
    /// The post-commit check finished.
    Verified,
}

/// Passes flash events through, and remembers how far the flash got — which
/// is what decides what a failure means.
struct Tracker<'o> {
    observer: &'o mut dyn Observer,
    reached: Reached,
    verification: Option<Verification>,
}

impl flows::Observer for Tracker<'_> {
    fn event(&mut self, event: Event<'_>) {
        match event {
            Event::Written { .. } => self.reached = Reached::Written,
            Event::Committed { .. } => self.reached = Reached::Committed,
            Event::Verified(v) => {
                self.reached = Reached::Verified;
                self.verification = Some(v);
            }
            _ => {}
        }
        self.observer.progress(Progress::Flash(event));
    }

    fn cancelled(&mut self) -> bool {
        self.observer.cancelled()
    }
}

/// What a session with a bootloader came to. In every case the device has
/// been asked to boot.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Ended {
    /// Nothing was committed; the previous image is what the device boots.
    NotCommitted(Stopped),
    /// The image linked for `base` was committed.
    Committed {
        base: u32,
        verification: Option<Verification>,
        problem: Option<String>,
    },
    /// COMMIT's reply never came and the device could not be asked what
    /// happened. Which build comes back is the only evidence left. It was
    /// still asked to boot, without knowing whether it heard.
    Unknown { base: u32, why: String },
}

/// Ask the device to start whatever it can boot. Mid-update that is the
/// previous application: the slot being written was never the active one.
/// Errors are ignored — without a session this is refused, harmlessly, and
/// with one it either works or leaves nothing more to try.
fn boot(client: &mut BootClient) {
    let _ = client.boot(BootMode::App);
}

/// Did the device give a definite "no" to COMMIT, as opposed to no answer?
fn commit_definitely_failed(e: &anyhow::Error) -> bool {
    e.downcast_ref::<VerifyMismatch>().is_some()
        || matches!(
            e.downcast_ref::<ClientError>(),
            Some(ClientError::Device { .. })
        )
}

/// One session with an OpenBoot bootloader that is already open: HELLO, pick
/// the build for the slot it offers, flash it, and boot — with every failure
/// settled into an [`Ended`] and the device always sent back.
///
/// This is the part of an update that is the same whatever the device is and
/// however it was reached: a dongle over USB after its hand-off, or a
/// keyboard's module through its tunnel. `serial`, where the transport has
/// one, must match the UID the bootloader reports.
pub fn flash_session(
    transport: &mut dyn Transport,
    serial: Option<&str>,
    source: &Source,
    observer: &mut dyn Observer,
) -> Ended {
    let mut client = BootClient::new(transport);
    let mut tracker = Tracker {
        observer,
        reached: Reached::Nothing,
        verification: None,
    };
    let opts = FlashOpts {
        force: true,
        verify: true,
        boot: true,
    };

    // HELLO, pick the build for the slot the device offers, check it fits.
    // From here the device will not leave the bootloader on its own.
    let job = match flows::prepare_flash(&mut client, source, &opts, &mut tracker) {
        Ok(job) => job,
        Err(e) => {
            boot(&mut client);
            return Ended::NotCommitted(Stopped::Failed(format!("{e:#}")));
        }
    };

    // The interface that was opened must be the bootloader now answering:
    // its USB serial is built from the UID it reports.
    if let Some(serial) = serial {
        if !serial.eq_ignore_ascii_case(&job.info.uid_hex()) {
            boot(&mut client);
            return Ended::NotCommitted(Stopped::Failed(format!(
                "the bootloader opened has USB serial {serial} but answers HELLO as {}; \
                 refusing to write to it",
                job.info.uid_hex()
            )));
        }
    }

    tracker.observer.progress(Progress::Stage(Stage::Flashing));
    let base = job.plan.base;
    let e = match flows::execute_flash(&mut client, &job, &mut tracker) {
        Ok(done) => {
            return Ended::Committed {
                base,
                verification: Some(done.verification),
                problem: None,
            }
        }
        Err(e) => e,
    };
    let why = format!("{e:#}");

    match tracker.reached {
        // COMMIT was acknowledged and the check finished: only BOOT can have
        // failed, or lost its reply. Never send it again in this session —
        // the reply being lost is what a successful boot looks like.
        Reached::Verified => Ended::Committed {
            base,
            verification: tracker.verification,
            problem: Some(why),
        },
        // COMMIT was acknowledged, then the live CRC cross-check failed or
        // could not be read. The record is written either way. Start it: the
        // bootloader checks the image again at boot and falls back to the
        // previous one if it is bad, and what comes back says which happened.
        Reached::Committed => {
            boot(&mut client);
            Ended::Committed {
                base,
                verification: None,
                problem: Some(why),
            }
        }
        // Everything was written and the failure is COMMIT's own — or a
        // cancel that arrived just before it was sent.
        Reached::Written => {
            if e.downcast_ref::<Cancelled>().is_some() {
                boot(&mut client);
                return Ended::NotCommitted(Stopped::Cancelled);
            }
            if commit_definitely_failed(&e) {
                boot(&mut client);
                return Ended::NotCommitted(Stopped::Failed(why));
            }
            // No answer. The commit may have landed with its reply lost. A
            // fresh HELLO settles it: a commit swaps the slots, so the slot
            // that was offered for writing is now the active one.
            match client.hello() {
                Ok(now) if now.active_slot == job.info.write_slot => {
                    boot(&mut client);
                    Ended::Committed {
                        base,
                        verification: None,
                        problem: Some(format!("{why}; a fresh HELLO showed the commit had landed")),
                    }
                }
                Ok(_) => {
                    boot(&mut client);
                    Ended::NotCommitted(Stopped::Failed(format!(
                        "{why}; a fresh HELLO showed nothing was committed"
                    )))
                }
                // Still nothing. Whether the commit landed cannot be known
                // from here, but the device may well be listening: a HELLO
                // it handled has opened a session even if its reply was
                // lost, and it would then sit in the bootloader for good.
                // Ask it to boot regardless. This is the first BOOT of the
                // session, and without a session it is refused, harmlessly.
                Err(_) => {
                    boot(&mut client);
                    Ended::Unknown { base, why }
                }
            }
        }
        // Still erasing or writing: nothing is committed.
        Reached::Nothing => {
            boot(&mut client);
            if e.downcast_ref::<Cancelled>().is_some() {
                Ended::NotCommitted(Stopped::Cancelled)
            } else {
                Ended::NotCommitted(Stopped::Failed(why))
            }
        }
    }
}

/* --- confirming --------------------------------------------------------- */

struct Waiting<'a> {
    request: &'a Request<'a>,
    before: &'a Identity,
    /// Where the bootloader was, for the one extra BOOT.
    bootloader_path: &'a str,
}

/// Wait for the addressed dongle's application to answer, and read what it
/// is running.
///
/// Another dongle answering is not that: with a second one attached, it is
/// the first thing found on every poll while the addressed one is still
/// resetting. So the wait goes on until the UID that was read before the
/// update shows up, or the deadline passes. Only then is a different unit
/// reported, as what was there instead.
fn who_came_back(
    waiting: &Waiting,
    bus: &mut dyn Bus,
    clock: &mut dyn Clock,
    devices: &mut dyn Devices,
) -> Option<Identity> {
    let target = &waiting.request.target;
    let started = clock.now();
    let mut rescued = false;
    let mut someone_else = None;
    loop {
        clock.sleep(POLL);
        for dev in devices.open_applications(target.vid, target.pid, waiting.request.interface) {
            match Identity::read(&dev) {
                Ok(identity) if identity.uid == waiting.before.uid => return Some(identity),
                Ok(identity) => someone_else = Some(identity),
                Err(_) => {}
            }
        }
        let waited = clock.now().duration_since(started);

        // Nothing yet, and a bootloader is still sitting there: the BOOT
        // never reached it, or it was never opened. One fresh session, one
        // BOOT. This is a new session, so it is not a resend. A poll on
        // which the bootloader could not be opened has not used that up.
        if !rescued && waited >= RESCUE_AFTER {
            let still_there = bus.scan().is_ok_and(|entries| {
                handoff::bootloader_paths(&entries, target.vid, target.pid)
                    .iter()
                    .any(|p| p == waiting.bootloader_path)
            });
            if still_there {
                if let Ok(mut bootloader) =
                    devices.open_bootloader(target.vid, target.pid, waiting.bootloader_path)
                {
                    rescued = true;
                    let mut client = BootClient::new(bootloader.transport.as_mut());
                    // The HELLO opens the session when the device handles it,
                    // not when its reply arrives. So the BOOT follows whatever
                    // came back: sending a HELLO and stopping there would
                    // strand a bootloader that had been about to idle out.
                    let _ = client.hello();
                    boot(&mut client);
                    let _ = bootloader.transport.close();
                }
            }
        }
        if waited >= RETURN_DEADLINE {
            return someone_else;
        }
    }
}

fn judge(came_back: Option<Identity>, before: &Identity, expected_build: u32) -> Confirmation {
    match came_back {
        None => Confirmation::NoApplication,
        Some(id) if id.uid != before.uid => Confirmation::DifferentUnit(id),
        Some(id) if id.build_id == expected_build => Confirmation::Confirmed(id),
        Some(id) => Confirmation::WrongBuild(id),
    }
}

/* --- the transaction ---------------------------------------------------- */

/// Update the dongle behind `dev` with `request.artifact`.
///
/// `Err` is for a failure that leaves the dongle running its application as
/// before: the status read, the bus scan ahead of the reboot, or a reboot
/// request it refused or never acted on. It is also what a bus that cannot
/// be read while watching for the bootloader comes out as; then the dongle
/// may have left, and idles back by itself, since nothing has opened a
/// session on it. From the moment a bootloader is opened, every ending is an
/// [`Outcome`], because by then "it failed" is not enough to act on: the
/// caller needs to know what the dongle is doing now.
pub fn run(
    dev: IapDevice,
    request: &Request,
    bus: &mut dyn Bus,
    clock: &mut dyn Clock,
    devices: &mut dyn Devices,
    observer: &mut dyn Observer,
) -> Result<Outcome> {
    let artifact = request.artifact;
    let target = &request.target;

    // Who is this, and is the bundle for it? Unarmed: nothing is disturbed.
    let before = Identity::read(&dev).context("reading the dongle's status")?;
    if before.family != artifact.family {
        return Ok(Outcome::Refused(format!(
            "the bundle is for {} (family 0x{:02X}) but this dongle reports family 0x{:02X}",
            artifact.chip_name(),
            artifact.family,
            before.family
        )));
    }

    observer.progress(Progress::Stage(Stage::Rebooting));
    let arrived = handoff::enter_and_watch(dev, target, bus, clock, &mut || {
        observer.progress(Progress::RebootAccepted)
    })?;
    let Handoff::Arrived { path } = arrived else {
        return Ok(Outcome::HandoffFailed(arrived));
    };
    let waiting = Waiting {
        request,
        before: &before,
        bootloader_path: &path,
    };

    observer.progress(Progress::Stage(Stage::Opening));
    let ended = match devices.open_bootloader(target.vid, target.pid, &path) {
        Ok(mut bootloader) => {
            let ended = flash_session(
                bootloader.transport.as_mut(),
                bootloader.serial.as_deref(),
                artifact.source(),
                observer,
            );
            let _ = bootloader.transport.close();
            ended
        }
        // Never opened, so no HELLO was sent: its idle timer is still running
        // and returns it to the application by itself.
        Err(e) => Ended::NotCommitted(Stopped::Failed(format!(
            "could not open the bootloader at {path}: {e:#}"
        ))),
    };

    observer.progress(Progress::Stage(Stage::Confirming));
    let came_back = who_came_back(&waiting, bus, clock, devices);
    let new_build = |base: u32| {
        artifact
            .variant_at(base)
            .expect("the flash wrote one of the bundle's builds")
            .build_id
    };

    Ok(match ended {
        Ended::NotCommitted(why) => Outcome::NotCommitted {
            why,
            back: judge(came_back, waiting.before, before.build_id),
        },
        Ended::Committed {
            base,
            verification,
            problem,
        } => Outcome::Committed {
            verification,
            problem,
            running: judge(came_back, waiting.before, new_build(base)),
        },
        // Nothing but the result to go on. The addressed dongle running the
        // new build means the commit landed. Running its previous build means
        // the update is not what it boots, whatever became of the commit.
        // Anything else settles nothing, and is reported as exactly that: to
        // say "not applied" would be a guess, and the dongle may come up on
        // the new build the next time it is plugged in.
        Ended::Unknown { base, why } => {
            let as_new = judge(came_back, waiting.before, new_build(base));
            let as_old = judge(came_back, waiting.before, before.build_id);
            if matches!(as_new, Confirmation::Confirmed(_)) {
                Outcome::Committed {
                    verification: None,
                    problem: Some(why),
                    running: as_new,
                }
            } else if matches!(as_old, Confirmation::Confirmed(_)) {
                Outcome::NotCommitted {
                    why: Stopped::Failed(format!(
                        "{why}; the device then stopped answering, and came back on \
                         its previous firmware"
                    )),
                    back: as_old,
                }
            } else {
                Outcome::Unknown { why, back: as_old }
            }
        }
    })
}
