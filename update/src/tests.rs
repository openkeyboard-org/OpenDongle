// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! The transaction, driven against the simulated dongle: the path that works,
//! and every way it can be interrupted.

use openboot::flows::{Event, Phase, Verification};
use openboot::proto::consts::{
    OB_CMD_BOOT, OB_CMD_COMMIT, OB_CMD_CRC, OB_CMD_ERASE, OB_CMD_HELLO, OB_CMD_WRITE,
    OB_FAMILY_CH570,
};
use opendongle::handoff::{Handoff, Target};

use crate::artifact::tests::{bundle, ch592_bundle, CH570_BASES};
use crate::artifact::Artifact;
use crate::sim::*;
use crate::transaction::*;

/// The build the dongle starts on, and the release being installed: slot A's
/// build of it, and slot B's.
const OLD: u32 = 0x0100;
const NEW_A: u32 = 0x2000;
const NEW_B: u32 = 0x2001;

/// Records what was reported, and cancels when `stop_when` says so.
struct Recorder<F: FnMut(&[String]) -> bool> {
    seen: Vec<String>,
    stop_when: F,
}

impl<F: FnMut(&[String]) -> bool> Observer for Recorder<F> {
    fn progress(&mut self, progress: Progress<'_>) {
        self.seen.push(match progress {
            Progress::Stage(stage) => format!("{stage:?}"),
            Progress::RebootAccepted => "accepted".to_string(),
            Progress::Flash(Event::Progress { phase, done, .. }) => format!("{phase:?} {done}"),
            Progress::Flash(Event::Committed { .. }) => "committed".to_string(),
            Progress::Flash(_) => "flash".to_string(),
        });
    }

    fn cancelled(&mut self) -> bool {
        (self.stop_when)(&self.seen)
    }
}

fn update_with(world: &Shared, artifact: &Artifact, observer: &mut dyn Observer) -> Outcome {
    let request = Request {
        target: Target {
            path: APP_PATH.to_string(),
            vid: VID,
            pid: PID,
        },
        interface: 4,
        artifact,
    };
    run(
        application(world).1,
        &request,
        &mut SimBus(world.clone()),
        &mut SimClock::new(world),
        &mut SimDevices(world.clone()),
        observer,
    )
    .expect("the transaction reports an outcome")
}

fn release() -> Artifact {
    Artifact::parse(&ch592_bundle(NEW_A)).unwrap()
}

fn update(world: &Shared) -> Outcome {
    update_with(world, &release(), &mut ())
}

fn confirmed_build(c: &Confirmation) -> u32 {
    match c {
        Confirmation::Confirmed(id) => id.build_id,
        other => panic!("expected a confirmed dongle, got {other:?}"),
    }
}

/* --- the path that works ------------------------------------------------ */

#[test]
fn an_update_ends_with_the_addressed_dongle_running_the_new_build() {
    let world = World::running(OLD);
    let mut rec = Recorder {
        seen: Vec::new(),
        stop_when: |_: &[String]| false,
    };
    let outcome = update_with(&world, &release(), &mut rec);

    assert!(outcome.succeeded(), "{outcome:?}");
    let Outcome::Committed {
        verification,
        problem,
        running,
    } = outcome
    else {
        unreachable!()
    };
    assert!(matches!(verification, Some(Verification::LiveCrc { .. })));
    assert_eq!(problem, None);
    // Slot A was running, so the device offered slot B and got B's build.
    assert_eq!(confirmed_build(&running), NEW_B);

    let w = world.borrow();
    assert_eq!(w.mode, Mode::App);
    assert_eq!(w.slots, [Some(OLD), Some(NEW_B)], "slot A is untouched");
    // One session: HELLO, one erase block, the image, COMMIT, CRC, BOOT.
    assert_eq!(w.obp.first(), Some(&OB_CMD_HELLO));
    assert_eq!(w.sent(OB_CMD_HELLO), 1);
    assert_eq!(w.sent(OB_CMD_ERASE), 1);
    assert_eq!(w.sent(OB_CMD_WRITE), 0x1000usize.div_ceil(48));
    assert_eq!(
        w.obp[w.obp.len() - 3..],
        [OB_CMD_COMMIT, OB_CMD_CRC, OB_CMD_BOOT]
    );

    let stages: Vec<&str> = rec
        .seen
        .iter()
        .map(String::as_str)
        .filter(|s| !s.starts_with("Write") && !s.starts_with("Erase") && *s != "flash")
        .collect();
    assert_eq!(
        stages,
        [
            "Rebooting",
            "accepted",
            "Opening",
            "Flashing",
            "committed",
            "Confirming"
        ]
    );
}

#[test]
fn the_next_update_goes_to_the_other_slot_and_expects_that_slots_build() {
    let world = World::running(OLD);
    assert!(update(&world).succeeded());

    // A later release. Slot B is running now, so slot A is offered.
    let later = Artifact::parse(&ch592_bundle(0x3000)).unwrap();
    let outcome = update_with(&world, &later, &mut ());
    let Outcome::Committed { running, .. } = outcome else {
        panic!("{outcome:?}")
    };
    assert_eq!(confirmed_build(&running), 0x3000);
    assert_eq!(world.borrow().slots, [Some(0x3000), Some(NEW_B)]);
}

/* --- refused before anything changes ------------------------------------ */

#[test]
fn a_bundle_for_another_chip_is_refused_while_the_application_still_runs() {
    let world = World::running(OLD);
    let ch570 = Artifact::parse(&bundle(OB_FAMILY_CH570, 0x70, CH570_BASES, NEW_A)).unwrap();
    let outcome = update_with(&world, &ch570, &mut ());
    let Outcome::Refused(why) = outcome else {
        panic!("{outcome:?}")
    };
    assert!(why.contains("CH570"), "{why}");
    let w = world.borrow();
    assert_eq!(w.mode, Mode::App, "the dongle was never rebooted");
    assert!(w.obp.is_empty());
}

#[test]
fn a_bootloader_already_on_the_bus_stops_the_update_before_the_reboot() {
    let world = World::running(OLD);
    world.borrow_mut().faults.bootloader_already_present = true;
    assert_eq!(
        update(&world),
        Outcome::HandoffFailed(Handoff::BootloadersPresent { count: 1 })
    );
    assert_eq!(world.borrow().mode, Mode::App);
}

#[test]
fn a_dongle_that_refuses_to_reboot_is_an_error_and_keeps_running() {
    let world = World::running(OLD);
    world.borrow_mut().faults.refuse_reboot = true;
    let request = Request {
        target: Target {
            path: APP_PATH.to_string(),
            vid: VID,
            pid: PID,
        },
        interface: 4,
        artifact: &release(),
    };
    let result = run(
        application(&world).1,
        &request,
        &mut SimBus(world.clone()),
        &mut SimClock::new(&world),
        &mut SimDevices(world.clone()),
        &mut (),
    );
    assert!(result.is_err());
    assert_eq!(world.borrow().mode, Mode::App);
}

/* --- stopped before COMMIT ---------------------------------------------- */

fn not_committed(outcome: Outcome) -> (Stopped, Confirmation) {
    match outcome {
        Outcome::NotCommitted { why, back } => (why, back),
        other => panic!("expected an update that was not committed, got {other:?}"),
    }
}

#[test]
fn cancelling_mid_write_sends_the_dongle_back_to_what_it_was_running() {
    let world = World::running(OLD);
    let mut rec = Recorder {
        seen: Vec::new(),
        stop_when: |seen: &[String]| seen.iter().any(|s| s == "Write 96"),
    };
    let (why, back) = not_committed(update_with(&world, &release(), &mut rec));
    assert_eq!(why, Stopped::Cancelled);
    assert_eq!(confirmed_build(&back), OLD);

    let w = world.borrow();
    assert_eq!(w.sent(OB_CMD_WRITE), 2);
    assert_eq!(
        w.sent(OB_CMD_COMMIT),
        0,
        "a cancelled update is never committed"
    );
    assert_eq!(w.sent(OB_CMD_BOOT), 1, "and the dongle is always sent back");
    assert_eq!(w.slots, [Some(OLD), None]);
}

#[test]
fn a_cancel_after_the_last_write_still_stops_before_commit() {
    let world = World::running(OLD);
    let mut rec = Recorder {
        seen: Vec::new(),
        stop_when: |seen: &[String]| {
            seen.iter()
                .any(|s| s == &format!("{:?} 4096", Phase::Write))
        },
    };
    let (why, back) = not_committed(update_with(&world, &release(), &mut rec));
    assert_eq!(why, Stopped::Cancelled);
    assert_eq!(confirmed_build(&back), OLD);
    assert_eq!(world.borrow().sent(OB_CMD_COMMIT), 0);
}

#[test]
fn a_cancel_that_arrives_after_commit_does_not_stop_the_update() {
    let world = World::running(OLD);
    let mut rec = Recorder {
        seen: Vec::new(),
        stop_when: |seen: &[String]| seen.iter().any(|s| s == "committed"),
    };
    assert!(update_with(&world, &release(), &mut rec).succeeded());
}

#[test]
fn a_write_the_device_refuses_ends_the_update_and_restores_the_old_build() {
    let world = World::running(OLD);
    world.borrow_mut().faults.fail_write = Some(5);
    let (why, back) = not_committed(update(&world));
    assert!(matches!(why, Stopped::Failed(_)), "{why:?}");
    assert_eq!(confirmed_build(&back), OLD);
    assert_eq!(world.borrow().sent(OB_CMD_BOOT), 1);
}

#[test]
fn a_bootloader_whose_serial_and_hello_disagree_is_never_written() {
    let world = World::running(OLD);
    world.borrow_mut().faults.wrong_serial = true;
    let (why, back) = not_committed(update(&world));
    let Stopped::Failed(text) = why else {
        panic!("{why:?}")
    };
    assert!(text.contains("refusing to write"), "{text}");
    assert_eq!(confirmed_build(&back), OLD);
    let w = world.borrow();
    assert_eq!(w.sent(OB_CMD_ERASE), 0);
    assert_eq!(w.obp, [OB_CMD_HELLO, OB_CMD_BOOT]);
}

#[test]
fn a_bootloader_that_cannot_be_opened_idles_back_to_the_application() {
    // No HELLO was sent, so its own timer is what returns it.
    let world = World::running(OLD);
    world.borrow_mut().faults.cannot_open_bootloader = true;
    let (why, back) = not_committed(update(&world));
    assert!(matches!(why, Stopped::Failed(_)));
    assert_eq!(confirmed_build(&back), OLD);
    assert!(world.borrow().obp.is_empty());
}

/* --- COMMIT ------------------------------------------------------------- */

#[test]
fn a_commit_the_device_rejects_is_not_committed() {
    let world = World::running(OLD);
    world.borrow_mut().faults.commit = CommitFault::Mismatch;
    let (why, back) = not_committed(update(&world));
    assert!(matches!(why, Stopped::Failed(_)));
    assert_eq!(confirmed_build(&back), OLD);
}

#[test]
fn a_commit_whose_reply_is_lost_is_settled_by_asking_the_device() {
    let world = World::running(OLD);
    world.borrow_mut().faults.commit = CommitFault::ReplyLost;
    let outcome = update(&world);
    assert!(outcome.succeeded(), "{outcome:?}");
    let Outcome::Committed {
        verification,
        problem,
        running,
    } = outcome
    else {
        unreachable!()
    };
    assert_eq!(confirmed_build(&running), NEW_B);
    // It is not reported as verified: the cross-check never ran.
    assert_eq!(verification, None);
    assert!(problem.unwrap().contains("the commit had landed"));
    // Settled with a second HELLO, then booted.
    assert_eq!(world.borrow().sent(OB_CMD_HELLO), 2);
}

#[test]
fn a_commit_that_never_arrived_is_found_not_to_have_landed() {
    let world = World::running(OLD);
    world.borrow_mut().faults.commit = CommitFault::NeverArrives;
    let (why, back) = not_committed(update(&world));
    let Stopped::Failed(text) = why else {
        panic!("{why:?}")
    };
    assert!(text.contains("nothing was committed"), "{text}");
    assert_eq!(confirmed_build(&back), OLD);
    assert_eq!(world.borrow().slots, [Some(OLD), None]);
}

#[test]
fn a_device_that_goes_silent_after_commit_is_judged_by_what_comes_back() {
    // The commit landed, then nothing answered — not even the HELLO that
    // would have settled it. The dongle is power-cycled and starts the new
    // build, and that is the evidence.
    let world = World::running(OLD);
    world.borrow_mut().faults.commit = CommitFault::LandsThenSilent;
    let outcome = update(&world);
    assert!(outcome.succeeded(), "{outcome:?}");
    let Outcome::Committed { problem, .. } = outcome else {
        unreachable!()
    };
    assert!(problem.is_some());
}

#[test]
fn a_failed_cross_check_after_commit_is_reported_with_what_actually_runs() {
    let world = World::running(OLD);
    world.borrow_mut().faults.live_crc_wrong = true;
    let outcome = update(&world);
    let Outcome::Committed {
        verification,
        problem,
        running,
    } = outcome
    else {
        panic!("{outcome:?}")
    };
    assert_eq!(
        verification, None,
        "a failed check must not read as verified"
    );
    assert!(problem.unwrap().contains("verify mismatch"));
    // The bootloader's own boot-time check accepted it here.
    assert_eq!(confirmed_build(&running), NEW_B);
}

#[test]
fn a_committed_image_the_bootloader_then_rejects_shows_as_the_old_build() {
    let world = World::running(OLD);
    {
        let mut w = world.borrow_mut();
        w.faults.live_crc_wrong = true;
        w.faults.new_image_fails_boot_check = true;
    }
    let outcome = update(&world);
    assert!(!outcome.succeeded());
    let Outcome::Committed { running, .. } = outcome else {
        panic!("{outcome:?}")
    };
    assert!(
        matches!(running, Confirmation::WrongBuild(id) if id.build_id == OLD),
        "{running:?}"
    );
}

/* --- BOOT, and what came back ------------------------------------------- */

#[test]
fn a_lost_boot_reply_is_not_followed_by_a_second_boot() {
    let world = World::running(OLD);
    world.borrow_mut().faults.boot = BootFault::ReplyLost;
    let outcome = update(&world);
    assert!(outcome.succeeded(), "{outcome:?}");
    let w = world.borrow();
    assert_eq!(w.sent(OB_CMD_BOOT), 1, "BOOT must never be resent");
    assert_eq!(w.sent(OB_CMD_HELLO), 1);
}

#[test]
fn a_boot_that_never_arrived_gets_one_fresh_session_and_one_boot() {
    let world = World::running(OLD);
    world.borrow_mut().faults.boot = BootFault::FirstNeverArrives;
    let outcome = update(&world);
    assert!(outcome.succeeded(), "{outcome:?}");
    let w = world.borrow();
    // The second HELLO is the fresh session; the BOOT in it is the only one
    // the device ever received.
    assert_eq!(w.sent(OB_CMD_HELLO), 2);
    assert_eq!(w.sent(OB_CMD_BOOT), 1);
    assert_eq!(w.running_build(), Some(NEW_B));
}

#[test]
fn a_different_dongle_answering_afterwards_is_not_mistaken_for_success() {
    let world = World::running(OLD);
    world.borrow_mut().faults.another_dongle_returns = true;
    let outcome = update(&world);
    assert!(!outcome.succeeded());
    let Outcome::Committed { running, .. } = outcome else {
        panic!("{outcome:?}")
    };
    assert!(
        matches!(running, Confirmation::DifferentUnit(id) if id.uid == OTHER_UID),
        "{running:?}"
    );
}

#[test]
fn an_application_that_never_comes_back_is_reported_not_assumed() {
    let world = World::running(OLD);
    world.borrow_mut().faults.application_never_returns = true;
    let outcome = update(&world);
    assert!(!outcome.succeeded());
    assert!(matches!(
        outcome,
        Outcome::Committed {
            running: Confirmation::NoApplication,
            ..
        }
    ));
}
