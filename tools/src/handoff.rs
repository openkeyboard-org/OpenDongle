// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! Taking a dongle from its application into the OpenBoot bootloader, and
//! watching the bus to learn whether THAT dongle arrived.
//!
//! The reboot is a re-enumeration: the application's five interfaces leave and
//! one bootloader interface appears, under the same VID:PID. Nothing carries
//! across it — the application presents no USB serial — so "it worked" can
//! only be inferred from what left and what came. The inference here is
//! deliberately narrow. It succeeds only when the interface that was addressed
//! has gone, exactly one bootloader that was not there before has come, no
//! second one was ever seen, and that picture has held still for a moment.
//! Anything else is reported as what it was, never rounded up to success.
//!
//! Even a success identifies an interface path, not a physical unit, and
//! today nothing better is available: the two modes do not report one
//! identity. The application's status UID is the ROM MAC plus a two-byte
//! checksum on both chips. OpenBoot's HELLO UID is the flash unique ID on
//! CH592 — a different number altogether — and on CH570 the bare MAC with
//! the last two bytes zero, so only CH570 can be matched, and only on those
//! six bytes. Until the firmware reports the same identity in both modes, a
//! caller that must not write the wrong unit has to keep one dongle attached.

use std::collections::BTreeSet;
use std::time::{Duration, Instant};

use anyhow::Result;
use hidapi::HidApi;

use crate::flows;
use crate::iap::IapDevice;

/// The bootloader's HID report descriptor: vendor usage page 0xFF00, usage
/// 0x01 (OpenBoot PROTOCOL.md §12).
pub const OB_USAGE_PAGE: u16 = 0xFF00;
pub const OB_USAGE: u16 = 0x0001;

/// How often the bus is looked at while waiting.
pub const POLL: Duration = Duration::from_millis(200);
/// How long a bootloader has to appear, from the reboot being accepted.
pub const DEADLINE: Duration = Duration::from_secs(10);
/// Consecutive polls the winning picture must hold before it is believed: a
/// second device could still be mid-enumeration, and the application could
/// still come back.
const SETTLE_POLLS: u32 = 3;

/* --- the bus ------------------------------------------------------------ */

/// One HID interface as enumeration reports it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BusEntry {
    pub path: String,
    pub vid: u16,
    pub pid: u16,
    /// -1 where the platform does not number interfaces.
    pub interface: i32,
    pub usage_page: u16,
    pub usage: u16,
}

impl BusEntry {
    fn is_bootloader(&self) -> bool {
        self.usage_page == OB_USAGE_PAGE && self.usage == OB_USAGE
    }
}

/// Something that can list the HID interfaces present right now.
pub trait Bus {
    fn scan(&mut self) -> Result<Vec<BusEntry>>;
}

/// Every interface a `HidApi` snapshot holds.
pub fn entries(api: &HidApi) -> Vec<BusEntry> {
    api.device_list()
        .map(|d| BusEntry {
            path: d.path().to_string_lossy().into_owned(),
            vid: d.vendor_id(),
            pid: d.product_id(),
            interface: d.interface_number(),
            usage_page: d.usage_page(),
            usage: d.usage(),
        })
        .collect()
}

/// The real bus. Each scan builds a fresh `HidApi`: its device list is a
/// snapshot taken at construction, so reusing one would never see a change.
pub struct HidBus;

impl Bus for HidBus {
    fn scan(&mut self) -> Result<Vec<BusEntry>> {
        Ok(entries(&HidApi::new()?))
    }
}

/// Time, as the watch uses it, so tests need neither a real clock nor a
/// real ten seconds.
pub trait Clock {
    fn now(&mut self) -> Instant;
    fn sleep(&mut self, d: Duration);
}

pub struct SystemClock;

impl Clock for SystemClock {
    fn now(&mut self) -> Instant {
        Instant::now()
    }
    fn sleep(&mut self, d: Duration) {
        std::thread::sleep(d);
    }
}

/* --- recognising the bootloader ----------------------------------------- */

/// Given every HID interface already filtered to one VID:PID, is one of them
/// the bootloader?
///
/// VID:PID alone stopped being enough when the dongle's board files gave
/// OpenBoot the application's own identity (0C45:FEFE). Both modes now
/// enumerate identically, and the application's keyboard, mouse and vendor
/// interfaces sit behind that same VID:PID. The vendor usage page 0xFF00 /
/// usage 0x01 is what separates them — the application deliberately uses
/// 0xFFFF and 0xFF60 instead (firmware/common/src/usb_descriptors.c).
///
/// FAIL CLOSED: only an exact vendor usage counts.
///
/// A 0/0 pair means the backend could not parse the report descriptor. It is
/// "cannot tell", and on this product cannot-tell is indistinguishable from
/// wrong: the application shares the bootloader's VID:PID, so a set of
/// application interfaces all reporting 0/0 would otherwise be classified as
/// OpenBoot and acted upon — rebooting or flashing against a running
/// application.
///
/// Earlier revisions accepted 0/0 when nothing reported a real usage, on the
/// grounds that a platform not reporting usages should still work. That trades
/// a hard failure for a wrong answer, which is the wrong way round here: the
/// caller uses this to decide which physical device to act on. On such a
/// platform the tool now says it cannot identify the bootloader rather than
/// guessing, and `--hidraw` remains available to name the device outright.
///
/// Stricter than OpenBoot's own `narrow_to_bootloader()`, which keeps the 0/0
/// fallback — it can afford to, because it returns a candidate list and errors
/// when more than one survives.
pub fn has_bootloader_usage(usages: &[(u16, u16)]) -> bool {
    usages
        .iter()
        .any(|&(page, usage)| page == OB_USAGE_PAGE && usage == OB_USAGE)
}

/// The paths of every OpenBoot interface on this VID:PID.
///
/// Paths rather than a count are what let the watch tell "the unit I
/// addressed arrived" from "some unit was already here": a count cannot
/// distinguish those.
pub fn bootloader_paths(entries: &[BusEntry], vid: u16, pid: u16) -> Vec<String> {
    entries
        .iter()
        .filter(|e| e.vid == vid && e.pid == pid && e.is_bootloader())
        .map(|e| e.path.clone())
        .collect()
}

/// The paths of every interface numbered `interface` on this VID:PID — one
/// per dongle running its application. More than one means discovery picked
/// among several, and nothing the user typed said which.
pub fn application_paths(entries: &[BusEntry], vid: u16, pid: u16, interface: i32) -> Vec<String> {
    entries
        .iter()
        .filter(|e| e.vid == vid && e.pid == pid && e.interface == interface)
        .map(|e| e.path.clone())
        .collect()
}

/* --- the watch ---------------------------------------------------------- */

/// The application interface being rebooted: the path that was opened, and
/// the identity it enumerates under.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Target {
    pub path: String,
    pub vid: u16,
    pub pid: u16,
}

impl Target {
    /// Is the addressed application interface still on the bus?
    ///
    /// Tracked by PATH, not by "anything with this VID:PID and interface":
    /// the latter is still true while a second dongle runs its application,
    /// which is a different device and says nothing about this one. A path
    /// can be handed to something new once it is free, so an entry at this
    /// path that is a bootloader does not count as the application.
    fn is_present(&self, entries: &[BusEntry]) -> bool {
        entries.iter().any(|e| {
            e.path == self.path && e.vid == self.vid && e.pid == self.pid && !e.is_bootloader()
        })
    }
}

/// How a hand-off ended.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Handoff {
    /// Refused before anything was sent: this many bootloaders were already
    /// on the bus, and the dongle was left running its application.
    BootloadersPresent { count: usize },
    /// The addressed application left and exactly one new bootloader came
    /// and stayed.
    Arrived { path: String },
    /// More than one new bootloader appeared; which is the addressed unit
    /// cannot be said.
    MultipleArrived { count: usize },
    /// A new bootloader appeared, but the addressed application is still
    /// there — so it is some other unit's.
    AppNeverLeft,
    /// A new bootloader appeared and then left again: it has most likely idled
    /// back into its application.
    BootloaderLeft,
    /// The application left and no bootloader appeared.
    NoBootloader,
    /// Nothing changed: the application is still on the bus.
    StillPresent,
}

/// The bootloaders already on the bus under the target's VID:PID. Anything
/// here is by definition not the unit about to be rebooted.
pub fn preexisting(bus: &mut dyn Bus, target: &Target) -> Result<BTreeSet<String>> {
    Ok(bootloader_paths(&bus.scan()?, target.vid, target.pid)
        .into_iter()
        .collect())
}

/// Watch the bus after the reboot was accepted, until the outcome is clear
/// or [`DEADLINE`] passes.
pub fn watch(
    target: &Target,
    preexisting: &BTreeSet<String>,
    bus: &mut dyn Bus,
    clock: &mut dyn Clock,
) -> Result<Handoff> {
    let deadline = clock.now() + DEADLINE;
    // Every new bootloader ever seen, and the ones there on the latest poll.
    // Both are needed: a second one that came and went still means the
    // picture was ambiguous, and one that came and went on its own means
    // there is nothing left to hand over.
    let mut ever: BTreeSet<String> = BTreeSet::new();
    let mut present: BTreeSet<String>;
    let mut app_present;
    let mut settled = 0u32;
    loop {
        clock.sleep(POLL);
        let entries = bus.scan()?;
        // Re-evaluated every poll, not latched: a momentarily absent
        // interface that comes back means the reboot did not stick.
        app_present = target.is_present(&entries);
        present = bootloader_paths(&entries, target.vid, target.pid)
            .into_iter()
            .filter(|p| !preexisting.contains(p))
            .collect();
        ever.extend(present.iter().cloned());

        let winning = !app_present && present.len() == 1 && ever.len() == 1;
        if winning {
            settled += 1;
            if settled >= SETTLE_POLLS {
                break;
            }
        } else {
            settled = 0;
        }
        // The deadline bounds the wait for a bootloader to APPEAR. One that
        // is there and settling is allowed the few polls it needs to finish;
        // the first poll that breaks the picture ends the watch.
        if !winning && clock.now() >= deadline {
            break;
        }
    }

    Ok(if settled >= SETTLE_POLLS {
        Handoff::Arrived {
            path: present.into_iter().next().expect("one bootloader present"),
        }
    } else if ever.len() > 1 {
        Handoff::MultipleArrived { count: ever.len() }
    } else if ever.len() == 1 && present.is_empty() {
        Handoff::BootloaderLeft
    } else if ever.len() == 1 {
        Handoff::AppNeverLeft
    } else if !app_present {
        Handoff::NoBootloader
    } else {
        Handoff::StillPresent
    })
}

/// The whole hand-off: check the bus is clear, reboot `dev`, watch.
///
/// Refuses, without sending anything, when a bootloader is already present:
/// the flash that follows could not be aimed (openboot rejects a multi-match),
/// so rebooting would cost a working dongle ten seconds for nothing.
///
/// `accepted` is called once the device has agreed to reboot, before the
/// wait, so a caller can say so while it happens.
pub fn enter_and_watch(
    dev: IapDevice,
    target: &Target,
    bus: &mut dyn Bus,
    clock: &mut dyn Clock,
    accepted: &mut dyn FnMut(),
) -> Result<Handoff> {
    let before = preexisting(bus, target)?;
    if !before.is_empty() {
        return Ok(Handoff::BootloadersPresent {
            count: before.len(),
        });
    }
    flows::enter_bootloader(&dev)?;
    accepted();
    drop(dev);
    watch(target, &before, bus, clock)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scripted::ScriptedLink;

    const VID: u16 = 0x0C45;
    const PID: u16 = 0xFEFE;

    fn app(path: &str) -> BusEntry {
        BusEntry {
            path: path.into(),
            vid: VID,
            pid: PID,
            interface: 4,
            usage_page: 0xFF60,
            usage: 0x0061,
        }
    }

    fn boot(path: &str) -> BusEntry {
        BusEntry {
            path: path.into(),
            vid: VID,
            pid: PID,
            interface: 0,
            usage_page: OB_USAGE_PAGE,
            usage: OB_USAGE,
        }
    }

    fn target() -> Target {
        Target {
            path: "app-1".into(),
            vid: VID,
            pid: PID,
        }
    }

    /// Plays back one bus picture per scan; the last repeats for ever.
    struct FakeBus {
        frames: Vec<Vec<BusEntry>>,
        scans: usize,
    }

    impl FakeBus {
        fn new(frames: Vec<Vec<BusEntry>>) -> FakeBus {
            FakeBus { frames, scans: 0 }
        }
    }

    impl Bus for FakeBus {
        fn scan(&mut self) -> Result<Vec<BusEntry>> {
            let at = self.scans.min(self.frames.len() - 1);
            self.scans += 1;
            Ok(self.frames[at].clone())
        }
    }

    /// Time that passes only when slept.
    struct FakeClock {
        now: Instant,
    }

    impl FakeClock {
        fn new() -> FakeClock {
            FakeClock {
                now: Instant::now(),
            }
        }
    }

    impl Clock for FakeClock {
        fn now(&mut self) -> Instant {
            self.now
        }
        fn sleep(&mut self, d: Duration) {
            self.now += d;
        }
    }

    fn run(frames: Vec<Vec<BusEntry>>) -> (Handoff, usize) {
        let mut bus = FakeBus::new(frames);
        let outcome = watch(&target(), &BTreeSet::new(), &mut bus, &mut FakeClock::new()).unwrap();
        (outcome, bus.scans)
    }

    const POLLS_TO_DEADLINE: usize = 50;

    #[test]
    fn the_addressed_dongle_arriving_is_a_success_once_it_has_settled() {
        let (outcome, scans) = run(vec![
            vec![app("app-1")],   // still resetting
            vec![],               // gone
            vec![boot("boot-9")], // arrived
        ]);
        assert_eq!(
            outcome,
            Handoff::Arrived {
                path: "boot-9".into()
            }
        );
        // Two polls to get there, then three that agree.
        assert_eq!(scans, 2 + SETTLE_POLLS as usize);
    }

    /// The defect this replaces: "app gone" used to mean "no interface 4 on
    /// this VID:PID at all", so a second dongle running its application made
    /// every hand-off fail after the reboot had already happened.
    #[test]
    fn another_dongle_running_its_application_does_not_hide_the_departure() {
        let (outcome, _) = run(vec![vec![app("app-2"), boot("boot-9")]]);
        assert_eq!(
            outcome,
            Handoff::Arrived {
                path: "boot-9".into()
            }
        );
    }

    #[test]
    fn a_bootloader_while_the_addressed_application_stays_is_not_ours() {
        let (outcome, scans) = run(vec![vec![app("app-1"), boot("boot-9")]]);
        assert_eq!(outcome, Handoff::AppNeverLeft);
        assert_eq!(scans, POLLS_TO_DEADLINE);
    }

    #[test]
    fn two_new_bootloaders_cannot_be_told_apart() {
        let (outcome, _) = run(vec![vec![boot("boot-8"), boot("boot-9")]]);
        assert_eq!(outcome, Handoff::MultipleArrived { count: 2 });
    }

    /// A second bootloader that came and went still made the picture
    /// ambiguous: the survivor may be the other unit. The old loop kept a
    /// running set and got this right; a per-poll snapshot would not.
    #[test]
    fn a_second_bootloader_that_came_and_went_still_counts() {
        let (outcome, _) = run(vec![
            vec![boot("boot-8"), boot("boot-9")],
            vec![boot("boot-9")],
        ]);
        assert_eq!(outcome, Handoff::MultipleArrived { count: 2 });
    }

    /// The other half of the old defect: arrivals were only ever added, so a
    /// bootloader that appeared and left again stayed "arrived" and the
    /// hand-off reported success for a device no longer in the bootloader.
    #[test]
    fn a_bootloader_that_left_again_is_not_handed_over() {
        let (outcome, _) = run(vec![
            vec![boot("boot-9")],
            vec![boot("boot-9")],
            vec![app("app-1")], // idled back into the application
        ]);
        assert_eq!(outcome, Handoff::BootloaderLeft);
    }

    #[test]
    fn settling_starts_again_when_the_picture_breaks() {
        let (outcome, scans) = run(vec![
            vec![boot("boot-9")],
            vec![boot("boot-9")],
            vec![app("app-1"), boot("boot-9")], // the application flickered back
            vec![boot("boot-9")],
        ]);
        assert_eq!(
            outcome,
            Handoff::Arrived {
                path: "boot-9".into()
            }
        );
        assert_eq!(scans, 3 + SETTLE_POLLS as usize);
    }

    /// The old loop accepted a bootloader seen on the very last poll with no
    /// settling at all. A late arrival now gets the polls it needs — and
    /// still has to hold for them.
    #[test]
    fn an_arrival_at_the_deadline_is_given_time_to_settle() {
        let mut frames = vec![vec![]; POLLS_TO_DEADLINE - 1];
        frames.push(vec![boot("boot-9")]);
        let (outcome, scans) = run(frames);
        assert_eq!(
            outcome,
            Handoff::Arrived {
                path: "boot-9".into()
            }
        );
        assert_eq!(scans, POLLS_TO_DEADLINE - 1 + SETTLE_POLLS as usize);
    }

    #[test]
    fn an_arrival_at_the_deadline_that_does_not_hold_is_not_accepted() {
        let mut frames = vec![vec![]; POLLS_TO_DEADLINE - 1];
        frames.push(vec![boot("boot-9")]);
        frames.push(vec![]);
        let (outcome, _) = run(frames);
        assert_eq!(outcome, Handoff::BootloaderLeft);
    }

    #[test]
    fn the_application_leaving_with_nothing_arriving_is_reported_as_such() {
        let (outcome, scans) = run(vec![vec![]]);
        assert_eq!(outcome, Handoff::NoBootloader);
        assert_eq!(scans, POLLS_TO_DEADLINE);
    }

    #[test]
    fn nothing_changing_is_reported_as_such() {
        let (outcome, _) = run(vec![vec![app("app-1")]]);
        assert_eq!(outcome, Handoff::StillPresent);
    }

    #[test]
    fn a_bootloader_that_was_there_before_is_never_the_arrival() {
        let before: BTreeSet<String> = ["boot-old".to_string()].into();
        let mut bus = FakeBus::new(vec![vec![boot("boot-old")]]);
        let outcome = watch(&target(), &before, &mut bus, &mut FakeClock::new()).unwrap();
        assert_eq!(outcome, Handoff::NoBootloader);
    }

    /// A freed path can be handed to whatever enumerates next. If the
    /// bootloader lands on the application's old path, that is the bootloader
    /// arriving, not the application staying.
    #[test]
    fn a_bootloader_reusing_the_application_path_is_not_the_application() {
        let (outcome, _) = run(vec![vec![boot("app-1")]]);
        assert_eq!(
            outcome,
            Handoff::Arrived {
                path: "app-1".into()
            }
        );
    }

    /* --- the whole hand-off ---------------------------------------------- */

    fn dongle() -> ScriptedLink {
        ScriptedLink::new(|cmd, body| {
            Some(match cmd {
                0x5A => vec![0xA5],
                0x84 if body[0] == 1 => vec![0x04, 0x0A, 0, 0, 0, 0, 0, 0, 0x92, 0, 0, 0],
                0x84 | 0x85 => vec![0x0F, 0x01, 0, 0],
                _ => return None,
            })
        })
    }

    #[test]
    fn a_bootloader_already_present_refuses_before_anything_is_sent() {
        let link = dongle();
        let mut bus = FakeBus::new(vec![vec![app("app-1"), boot("boot-old")]]);
        let mut accepted = 0;
        let outcome = enter_and_watch(
            link.device(),
            &target(),
            &mut bus,
            &mut FakeClock::new(),
            &mut || accepted += 1,
        )
        .unwrap();
        assert_eq!(outcome, Handoff::BootloadersPresent { count: 1 });
        assert!(link.commands().is_empty(), "the dongle must be left alone");
        assert_eq!(accepted, 0);
    }

    #[test]
    fn a_clear_bus_reboots_then_watches() {
        let link = dongle();
        let mut bus = FakeBus::new(vec![
            vec![app("app-1")],   // the pre-check
            vec![],               // gone
            vec![boot("boot-9")], // arrived
        ]);
        let mut accepted = 0;
        let outcome = enter_and_watch(
            link.device(),
            &target(),
            &mut bus,
            &mut FakeClock::new(),
            &mut || accepted += 1,
        )
        .unwrap();
        assert_eq!(
            outcome,
            Handoff::Arrived {
                path: "boot-9".into()
            }
        );
        assert_eq!(link.commands(), vec![0x5A, 0x84, 0x85]);
        assert_eq!(accepted, 1);
    }

    #[test]
    fn a_refused_reboot_is_an_error_and_the_bus_is_not_watched() {
        let link = ScriptedLink::new(|cmd, body| {
            Some(match cmd {
                0x5A => vec![0xA5],
                0x84 if body[0] == 1 => vec![0x04, 0x0A, 0, 0, 0, 0, 0, 0, 0x92, 0, 0, 0],
                0x84 => vec![0x0F, 0x01, 0, 0],
                0x85 => vec![0x0F, 0x01, 0xE2, 0],
                _ => return None,
            })
        });
        let mut bus = FakeBus::new(vec![vec![app("app-1")]]);
        let mut accepted = 0;
        let result = enter_and_watch(
            link.device(),
            &target(),
            &mut bus,
            &mut FakeClock::new(),
            &mut || accepted += 1,
        );
        assert!(result.is_err());
        assert_eq!(accepted, 0);
        assert_eq!(bus.scans, 1, "only the pre-check looked at the bus");
    }

    /* --- recognising the bootloader -------------------------------------- */

    // The dongle's bootloader shares the application's VID:PID, so these
    // pairs are the ONLY thing distinguishing the two modes. Getting this
    // wrong is silent in both directions: too permissive reports "already in
    // the bootloader" while the app is running, too strict makes
    // --enter-bootloader always report failure after a successful reboot.
    #[test]
    fn bootloader_usage_needs_the_exact_vendor_page() {
        // The bootloader's own interface.
        assert!(has_bootloader_usage(&[(0xFF00, 0x0001)]));
        // The application's interfaces, as declared in usb_descriptors.c.
        // None of these may pass.
        assert!(!has_bootloader_usage(&[(0xFFFF, 0x0001), (0xFF60, 0x0061)]));
        // Right page, wrong usage — and vice versa.
        assert!(!has_bootloader_usage(&[(0xFF00, 0x0002)]));
        assert!(!has_bootloader_usage(&[(0xFF01, 0x0001)]));
        // Nothing on the bus under this VID:PID.
        assert!(!has_bootloader_usage(&[]));
    }

    #[test]
    fn unknown_usage_never_stands_in_for_the_bootloader() {
        // 0/0 means the backend could not parse the report descriptor. On a
        // device whose application SHARES the bootloader's VID:PID, that is
        // indistinguishable from wrong, so it must never count - not even when
        // it is the only thing on the bus. Guessing here means rebooting or
        // flashing against a running application.
        assert!(!has_bootloader_usage(&[(0, 0)]));
        // Several application interfaces all reporting 0/0 is the concrete
        // case: without fail-closed behaviour this classified the running
        // application as OpenBoot.
        assert!(!has_bootloader_usage(&[(0, 0), (0, 0), (0, 0)]));
        assert!(!has_bootloader_usage(&[(0xFFFF, 0x0001), (0, 0)]));
        // Only the declared vendor usage counts, mixed in or not.
        assert!(has_bootloader_usage(&[(0, 0), (0xFF00, 0x0001)]));
    }

    #[test]
    fn bootloader_paths_are_only_those_declaring_the_usage_on_this_identity() {
        let unknown = BusEntry {
            usage_page: 0,
            usage: 0,
            ..app("unreadable")
        };
        let elsewhere = BusEntry {
            vid: 0x1209,
            pid: 0x0001,
            ..boot("other-product")
        };
        let bus = vec![app("app-1"), boot("boot-9"), unknown, elsewhere];
        assert_eq!(bootloader_paths(&bus, VID, PID), vec!["boot-9"]);
    }

    #[test]
    fn application_paths_count_dongles_not_interfaces() {
        let keyboard = BusEntry {
            interface: 0,
            usage_page: 1,
            usage: 6,
            ..app("kbd-1")
        };
        let bus = vec![keyboard, app("app-1"), app("app-2"), boot("boot-9")];
        assert_eq!(application_paths(&bus, VID, PID, 4), vec!["app-1", "app-2"]);
    }
}
