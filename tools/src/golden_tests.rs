// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! The CLI's text, pinned. Each case runs one command line against a scripted
//! dongle and compares everything it prints — stdout, stderr and the error
//! line `main` would add — with a file under `tests/golden/`.
//!
//! The decoders have their own tests, which pin what each renders; these pin
//! what a whole command says, in order, including on the paths where it stops
//! half-way. A change to the text is sometimes intended and should never be an
//! accident. To accept a deliberate one:
//!
//! ```text
//! UPDATE_GOLDEN=1 cargo test golden
//! ```
//!
//! and review the diff of `tests/golden/` like any other.

use std::cell::RefCell;
use std::io::Write;
use std::path::PathBuf;
use std::rc::Rc;

use clap::Parser;

use super::{device_commands, report_open_failure, Cli, Step};
use opendongle::console::{self, Console, HandoffContext};
use opendongle::handoff::Handoff;
use opendongle::scripted::ScriptedLink;

/* --- capture ------------------------------------------------------------ */

#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<u8>>>);

impl Write for Shared {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.0.borrow_mut().extend_from_slice(buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl Shared {
    fn text(&self) -> String {
        String::from_utf8(self.0.borrow().clone()).expect("console output is UTF-8")
    }
}

/// The sampler lines open with the wall clock; nothing else in the output
/// depends on when it ran.
fn mask_clock(text: &str) -> String {
    let is_stamp = |word: &str| {
        word.split_once('.').is_some_and(|(secs, millis)| {
            !secs.is_empty()
                && secs.bytes().all(|b| b.is_ascii_digit())
                && millis.len() == 3
                && millis.bytes().all(|b| b.is_ascii_digit())
        })
    };
    let mut out = String::new();
    for line in text.split_inclusive('\n') {
        let (lead, rest) = match line.strip_prefix("--- ") {
            Some(rest) => ("--- ", rest),
            None => ("", line),
        };
        match rest.split_once(' ') {
            Some((word, tail)) if is_stamp(word) => {
                out.push_str(lead);
                out.push_str("<clock> ");
                out.push_str(tail);
            }
            _ => out.push_str(line),
        }
    }
    out
}

/// Run `body` with a capturing console and render what it printed, followed
/// by the outcome line(s) it returns, as one transcript.
fn captured(body: impl FnOnce(&mut Console) -> String) -> String {
    let (out, err) = (Shared::default(), Shared::default());
    let mut con = Console {
        out: Box::new(out.clone()),
        err: Box::new(err.clone()),
    };
    let outcome = body(&mut con);
    drop(con);
    format!(
        "--- stdout ---\n{}--- stderr ---\n{}--- outcome ---\n{}",
        mask_clock(&out.text()),
        err.text(),
        outcome
    )
}

fn parse(args: &[&str]) -> Cli {
    Cli::try_parse_from(std::iter::once("opendongle").chain(args.iter().copied()))
        .expect("the test's command line parses")
}

/// Run `opendongle <args>` against `link` and render everything a user would
/// see as one transcript.
fn transcript(args: &[&str], link: &ScriptedLink) -> String {
    let cli = parse(args);
    let dev = link.device();
    captured(|con| match device_commands(&cli, &dev, con) {
        Ok(Step::Exit(code)) => format!("exit {code}\n"),
        Ok(Step::EnterBootloader) => "continues: reboot into the bootloader\n".to_string(),
        // `main` prints the error itself, without the cause chain.
        Err(e) => format!("ERROR: {e}\nexit 1\n"),
    })
}
fn check(name: &str, actual: String) {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("golden")
        .join(format!("{name}.txt"));
    if std::env::var_os("UPDATE_GOLDEN").is_some() {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(&path, &actual).unwrap();
        return;
    }
    let expected = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| {
            panic!(
                "{}: {e}\nrun `UPDATE_GOLDEN=1 cargo test golden` to create it",
                path.display()
            )
        })
        .replace("\r\n", "\n");
    assert!(
        expected == actual,
        "{name}: output differs from {}\n--- expected ---\n{expected}\n--- actual ---\n{actual}",
        path.display()
    );
}

/* --- a scripted dongle -------------------------------------------------- */

fn framed(ack: u8, payload: &[u8]) -> Vec<u8> {
    let mut reply = vec![ack, payload.len() as u8];
    reply.extend_from_slice(payload);
    reply
}

/// A bond record with its checksum filled in, as the firmware returns it
/// (link key zeroed).
fn bond_record(flags: u8) -> [u8; 48] {
    let mut raw = [0u8; 48];
    raw[0..4].copy_from_slice(&0x444E_4F42u32.to_le_bytes());
    raw[4] = 2;
    raw[5] = flags;
    raw[6..8].copy_from_slice(&28u16.to_le_bytes());
    raw[8..12].copy_from_slice(&0xAC12_34CEu32.to_le_bytes());
    raw[12..14].copy_from_slice(&600u16.to_le_bytes());
    raw[22..28].copy_from_slice(&[0x10, 0x20, 0x30, 0x40, 0x50, 0x60]);
    let checksum: u32 = raw[..44].iter().map(|&b| u32::from(b)).sum();
    raw[44..48].copy_from_slice(&checksum.to_le_bytes());
    raw
}

fn bond_reply(status: u8, record: &[u8; 48]) -> Vec<u8> {
    let mut reply = vec![0x88, 48, status];
    reply.extend_from_slice(record);
    reply
}

fn status_reply(connection: u8, rssi: i8) -> Vec<u8> {
    let mut p = [0u8; 32];
    p[0] = 1; // schema
    p[1] = 0x92; // family
    p[2] = connection;
    p[4] = 0x0F; // capabilities
    p[5] = 8; // uid length
    p[6] = 1; // product profile
    p[7] = rssi as u8;
    p[8..16].copy_from_slice(&[0xB2, 0x59, 0x21, 0x62, 0x32, 0xDC, 0x05, 0x98]);
    p[16..20].copy_from_slice(&0x1234_ABCDu32.to_le_bytes());
    p[20..24].copy_from_slice(&25_848u32.to_le_bytes());
    p[24..30].copy_from_slice(&[0xBA, 0x47, 0x8B, 0x72, 0xAB, 0x3C]);
    framed(0x91, &p)
}

/// A CH592 fault page: valid record, no fault, three boots, armed keeper.
fn fault_reply(valid: bool) -> Vec<u8> {
    let mut p = [0u8; 40];
    p[0] = 0x92;
    p[1] = 1;
    p[7] = 0x5C;
    p[37] = 0xC5;
    p[38] = 1;
    if valid {
        p[2] = 1;
        p[3] = 1;
        p[20..24].copy_from_slice(&3u32.to_le_bytes());
        p[36] = 3;
        p[39] = 0x5C;
    }
    framed(0x93, &p)
}

fn rf_page_reply(page: u8) -> Vec<u8> {
    if page > 4 {
        return vec![0x92, 0]; // this firmware has no power pages
    }
    let mut p = [0u8; 62];
    p[0] = 1; // page version
    p[1] = page;
    framed(0x92, &p)
}

fn crypt_reply(product_layout: bool) -> Vec<u8> {
    let mut p = vec![0u8; if product_layout { 42 } else { 28 }];
    p[0..4].copy_from_slice(&1234u32.to_le_bytes()); // verified
    for (i, drops) in [1u32, 0, 2, 0, 0].iter().enumerate() {
        p[8 + 4 * i..12 + 4 * i].copy_from_slice(&drops.to_le_bytes());
    }
    if product_layout {
        p[28..32].copy_from_slice(&3u32.to_le_bytes()); // aes redo
        p[32..36].copy_from_slice(&1u32.to_le_bytes()); // announce retry
        p[36] = 1; // KAT run
        p[37] = 0; // KAT fail
        p[38..40].copy_from_slice(&7u16.to_le_bytes()); // plaintext drop
    }
    framed(0x98, &p)
}

fn kex_reply() -> Vec<u8> {
    let mut p = [0u8; 22];
    p[0] = 4; // started
    p[1] = 3; // completed
    p[2] = 5; // last failure: timeout
    p[3] = 5; // state: done
    p[4..6].copy_from_slice(&900u16.to_le_bytes()); // entropy samples
    p[6] = 0; // commit failures
    p[7] = 1; // tried this link
    p[8..16].copy_from_slice(&[5, 0, 0x7F, 2, 9, 1, 0x0F, 3]); // keyboard status
    p[16] = 12; // frames received
    p[17] = 1; // failures
    p[18] = 2; // dongle x25519 phase
    p[19] = 6; // count
    p[20] = 2; // seeds received
    p[21] = 0x81; // one waiting, one used
    framed(0x9A, &p)
}

fn heap_reply() -> Vec<u8> {
    let mut p = [0u8; 28];
    for (i, word) in [4096u32, 1024, 3072, 512, 2048, 2000, 2096]
        .iter()
        .enumerate()
    {
        p[4 * i..4 * i + 4].copy_from_slice(&word.to_le_bytes());
    }
    framed(0x9B, &p)
}

fn kex_bench_reply() -> Vec<u8> {
    let mut p = [0u8; 32];
    p[0..4].copy_from_slice(&6_000_000u32.to_le_bytes()); // total cycles
    p[4..8].copy_from_slice(&30_000u32.to_le_bytes()); // longest step
    p[8..12].copy_from_slice(&18_000u32.to_le_bytes()); // shortest step
    p[12..16].copy_from_slice(&255u32.to_le_bytes()); // steps
    p[16..24].copy_from_slice(&[0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04]);
    p[24..28].copy_from_slice(&1_200u32.to_le_bytes()); // begin
    p[28..32].copy_from_slice(&2_400u32.to_le_bytes()); // finish
    framed(0x99, &p)
}

/// A healthy CH592 dongle, connected, with an encrypted bond.
fn healthy(cmd: u8, body: &[u8]) -> Option<Vec<u8>> {
    Some(match cmd {
        0x5A => vec![0xA5],
        0x84 if body.first() == Some(&1) => {
            vec![0x04, 0x0A, 0, 0, 0, 0, 0, 0, 0x92, 0x00, 0x00, 0x00]
        }
        0x84 => vec![0x0F, 0x01, 0x00, 0x00],
        0x85 => vec![0x0F, 0x01, 0x00, 0x00],
        0x87 => vec![0x0F, 0x01, 0x00],
        0x88 => bond_reply(0x02, &bond_record(0x03)),
        0x89 => vec![0x0F, 0x01, 0x00],
        0x90 => b"0.97.1\0".to_vec(),
        0x91 => status_reply(3, -58),
        0x92 => rf_page_reply(body[0]),
        0x93 => fault_reply(true),
        0x94 => vec![0x94, 0x02, 0x00, 0x03],
        0x98 => crypt_reply(true),
        0x99 => kex_bench_reply(),
        0x9A => kex_reply(),
        0x9B => heap_reply(),
        0x9C => vec![0x9C, 0x01, 0x00],
        _ => return None,
    })
}

/// `healthy`, except that `cmd` is answered by `reply` (`None`: not at all).
fn except(cmd: u8, reply: Option<Vec<u8>>) -> ScriptedLink {
    ScriptedLink::new(move |c, body| {
        if c == cmd {
            reply.clone()
        } else {
            healthy(c, body)
        }
    })
}

fn dongle() -> ScriptedLink {
    ScriptedLink::new(healthy)
}

/* --- readouts ----------------------------------------------------------- */

#[test]
fn golden_info() {
    let link = dongle();
    check("info", transcript(&[], &link));
    // --info arms exactly twice (probe, bond read) and disarms each time.
    let arms: Vec<u8> = link
        .log()
        .iter()
        .filter(|(cmd, _)| *cmd == 0x84)
        .map(|(_, body)| body[0])
        .collect();
    assert_eq!(arms, vec![1, 0, 1, 0]);
}

#[test]
fn golden_info_unpaired_and_unhealthy() {
    // No bond (the firmware returns zeros), an invalid fault record, waiting
    // for a keyboard, and no RSSI reading yet.
    let link = ScriptedLink::new(|cmd, body| match cmd {
        0x88 => Some(bond_reply(0x01, &[0u8; 48])),
        0x91 => Some(status_reply(1, 0)),
        0x93 => Some(fault_reply(false)),
        _ => healthy(cmd, body),
    });
    check(
        "info_unpaired_and_unhealthy",
        transcript(&["--info"], &link),
    );
}

#[test]
fn golden_info_stops_where_the_device_stops_answering() {
    // The handshake lands and the arm does not: what was learned is printed,
    // then the error.
    let link = ScriptedLink::new(|cmd, body| match cmd {
        0x84 => None,
        _ => healthy(cmd, body),
    });
    check("info_arm_times_out", transcript(&[], &link));
}

#[test]
fn golden_info_bond_disarm_unconfirmed() {
    // The second disarm (after the bond read) answers with something that is
    // not an acknowledgement.
    let mut disarms = 0;
    let link = ScriptedLink::new(move |cmd, body| match cmd {
        0x84 if body.first() == Some(&0) => {
            disarms += 1;
            Some(if disarms == 2 {
                vec![0x77, 0x01, 0x00, 0x00]
            } else {
                vec![0x0F, 0x01, 0x00, 0x00]
            })
        }
        _ => healthy(cmd, body),
    });
    check("info_bond_disarm_unconfirmed", transcript(&[], &link));
}

#[test]
fn golden_fault() {
    let link = dongle();
    check("fault", transcript(&["--fault"], &link));
    assert_eq!(link.commands(), vec![0x93], "--fault must arm nothing");
}

#[test]
fn golden_status_sampler() {
    let link = dongle();
    check(
        "status_sampler",
        transcript(&["--status", "--samples", "2", "--period-ms", "0"], &link),
    );
    assert_eq!(
        link.commands(),
        vec![0x91, 0x91],
        "--status must arm nothing"
    );
}

#[test]
fn golden_diag() {
    let link = dongle();
    check("diag", transcript(&["--diag"], &link));
    assert!(
        link.commands().iter().all(|&cmd| cmd == 0x92),
        "--diag must arm nothing"
    );
}

#[test]
fn golden_sampler_knobs_without_a_sampler() {
    check(
        "samples_without_a_sampler",
        transcript(&["--samples", "5"], &dongle()),
    );
}

/* --- armed operations --------------------------------------------------- */

#[test]
fn golden_rf_poke() {
    let link = dongle();
    check("rf_poke", transcript(&["--rf-poke", "1"], &link));
    assert_eq!(link.commands(), vec![0x84, 0x94, 0x84], "arm, poke, disarm");
}

#[test]
fn golden_rf_poke_disarm_unconfirmed() {
    // The poke ran, then the link failed on the disarm: the session may still
    // be armed, and saying only "done" would call the dongle clean.
    let link = dongle();
    link.break_at(3);
    check(
        "rf_poke_disarm_unconfirmed",
        transcript(&["--rf-poke", "1"], &link),
    );
    assert_eq!(
        link.commands(),
        vec![0x84, 0x94],
        "arm, poke; the disarm never arrived"
    );
}

#[test]
fn golden_rf_poke_refused() {
    let link = except(0x94, Some(vec![0x94, 0x02, 0xE1, 0x05]));
    check("rf_poke_refused", transcript(&["--rf-poke", "40"], &link));
}

#[test]
fn golden_clear_bond() {
    let link = dongle();
    check("clear_bond", transcript(&["--clear-bond"], &link));
    assert_eq!(
        link.commands(),
        vec![0x84, 0x89, 0x84],
        "arm, clear, disarm"
    );
}

#[test]
fn golden_clear_bond_failed() {
    let link = except(0x89, Some(vec![0x0F, 0x01, 0xB4]));
    check("clear_bond_failed", transcript(&["--clear-bond"], &link));
    assert_eq!(*link.commands().last().unwrap(), 0x84, "still disarmed");
}

const KEY: &str = "000102030405060708090a0b0c0d0e0f";

#[test]
fn golden_provision_link_key() {
    let link = dongle();
    check(
        "provision_link_key",
        transcript(&["--provision-link-key", KEY], &link),
    );
    // The record written back carries the key, the key flag, and a checksum
    // that covers both.
    let (_, written) = link
        .log()
        .into_iter()
        .find(|(cmd, _)| *cmd == 0x87)
        .unwrap();
    assert_eq!(&written[28..44], &(0u8..16).collect::<Vec<_>>()[..]);
    assert_eq!(written[5] & 0x02, 0x02);
    let sum: u32 = written[..44].iter().map(|&b| u32::from(b)).sum();
    assert_eq!(&written[44..48], &sum.to_le_bytes());
}

#[test]
fn golden_provision_link_key_deferred() {
    let link = except(0x87, Some(vec![0x0F, 0x01, 0xB5]));
    check(
        "provision_link_key_deferred",
        transcript(&["--provision-link-key", KEY], &link),
    );
}

#[test]
fn golden_provision_link_key_without_a_bond() {
    let link = except(0x88, Some(bond_reply(0x01, &[0u8; 48])));
    check(
        "provision_link_key_without_a_bond",
        transcript(&["--provision-link-key", KEY], &link),
    );
    assert_eq!(*link.commands().last().unwrap(), 0x84, "still disarmed");
    assert!(!link.commands().contains(&0x87), "nothing may be written");
}

/* --- link encryption ---------------------------------------------------- */

#[test]
fn golden_crypt_diag() {
    check("crypt_diag", transcript(&["--crypt-diag"], &dongle()));
}

#[test]
fn golden_crypt_diag_counters_only() {
    // Firmware with the short layout and neither the key-exchange nor the
    // heap command.
    let link = ScriptedLink::new(|cmd, body| match cmd {
        0x98 => Some(crypt_reply(false)),
        0x9A | 0x9B => None,
        _ => healthy(cmd, body),
    });
    check(
        "crypt_diag_counters_only",
        transcript(&["--crypt-diag"], &link),
    );
}

#[test]
fn golden_kex_bench() {
    check("kex_bench", transcript(&["--kex-bench"], &dongle()));
}

#[test]
fn golden_kex_bench_unsupported() {
    check(
        "kex_bench_unsupported",
        transcript(&["--kex-bench"], &except(0x99, None)),
    );
}

const SEED: &str = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

#[test]
fn golden_kex_seed() {
    let link = dongle();
    check("kex_seed", transcript(&["--kex-seed", SEED], &link));
    let (cmd, body) = link.log().pop().unwrap();
    assert_eq!(cmd, 0x9C);
    assert_eq!(body, (0u8..32).collect::<Vec<_>>());
}

#[test]
fn golden_kex_seed_from_this_computer() {
    let link = dongle();
    check("kex_seed_random", transcript(&["--kex-seed"], &link));
    let (_, body) = link.log().pop().unwrap();
    assert_eq!(body.len(), 32);
}

#[test]
fn golden_kex_seed_refused() {
    let link = except(0x9C, Some(vec![0x9C, 0x01, 0x02]));
    check("kex_seed_refused", transcript(&["--kex-seed", SEED], &link));
}

/* --- before the reboot -------------------------------------------------- */

#[test]
fn golden_enter_bootloader_needs_a_family_check() {
    let link = dongle();
    check(
        "enter_bootloader_unguarded",
        transcript(&["--enter-bootloader"], &link),
    );
    assert!(!link.commands().contains(&0x85), "no reboot was requested");
}

#[test]
fn golden_enter_bootloader_forced_passes_the_guards() {
    let link = dongle();
    check(
        "enter_bootloader_forced",
        transcript(&["--enter-bootloader", "--force"], &link),
    );
    assert!(
        !link.commands().contains(&0x85),
        "the reboot itself is run()'s to request"
    );
}

/* --- the hand-off, as reported ------------------------------------------ */

fn handoff(outcome: Handoff, bundle: Option<&str>) -> String {
    let bundle = bundle.map(std::path::Path::new);
    let cx = HandoffContext {
        vid: 0x0C45,
        pid: 0xFEFE,
        interface: 4,
        bundle,
    };
    captured(|con| {
        console::reboot_accepted(con);
        format!("exit {}\n", console::report_handoff(&outcome, &cx, con))
    })
}

#[test]
fn golden_handoff_arrived() {
    let arrived = || Handoff::Arrived {
        path: "/dev/hidraw7".into(),
    };
    check(
        "handoff_arrived",
        handoff(arrived(), Some("build/ch592-product.obb")),
    );
    // With no --bundle the line carries a placeholder to fill in.
    check("handoff_arrived_no_bundle", handoff(arrived(), None));
}

#[test]
fn golden_handoff_failures() {
    for (name, outcome) in [
        (
            "handoff_multiple_arrived",
            Handoff::MultipleArrived { count: 2 },
        ),
        ("handoff_app_never_left", Handoff::AppNeverLeft),
        ("handoff_bootloader_left", Handoff::BootloaderLeft),
        ("handoff_no_bootloader", Handoff::NoBootloader),
        ("handoff_still_present", Handoff::StillPresent),
    ] {
        check(name, handoff(outcome, None));
    }
}

#[test]
fn golden_handoff_refused_before_the_reboot() {
    // Nothing was sent, so there is no "accepted" line.
    let cx = HandoffContext {
        vid: 0x0C45,
        pid: 0xFEFE,
        interface: 4,
        bundle: None,
    };
    check(
        "handoff_bootloaders_present",
        captured(|con| {
            let outcome = Handoff::BootloadersPresent { count: 1 };
            format!("exit {}\n", console::report_handoff(&outcome, &cx, con))
        }),
    );
}

/* --- when the application cannot be opened ------------------------------ */

const NOT_FOUND: &str = "no HID device for VID=0x0C45 PID=0xFEFE interface=4";

fn open_failure(args: &[&str], msg: &str, boots: &[&str]) -> String {
    let cli = parse(args);
    let boots: Vec<String> = boots.iter().map(|b| b.to_string()).collect();
    captured(|con| format!("exit {}\n", report_open_failure(&cli, msg, &boots, con)))
}

#[test]
fn golden_no_dongle() {
    check("open_no_dongle", open_failure(&[], NOT_FOUND, &[]));
    // A bootloader on the bus does not change that for a readout: only
    // --enter-bootloader has anything to say about it.
    check(
        "open_no_dongle_but_a_bootloader",
        open_failure(&["--info"], NOT_FOUND, &["/dev/hidraw3"]),
    );
}

#[test]
fn golden_cannot_open() {
    check(
        "open_denied",
        open_failure(&[], "hidapi error: Permission denied", &[]),
    );
}

#[test]
fn golden_already_in_the_bootloader() {
    let boot = &["/dev/hidraw3"];
    check(
        "open_already_in_bootloader_forced",
        open_failure(
            &["--enter-bootloader", "--force", "--bundle", "fw.obb"],
            NOT_FOUND,
            boot,
        ),
    );
    // The family guard cannot run without the application: exit 3.
    check(
        "open_already_in_bootloader_image_unverified",
        open_failure(
            &["--enter-bootloader", "--image", "fw.bin"],
            NOT_FOUND,
            boot,
        ),
    );
    check(
        "open_several_bootloaders",
        open_failure(
            &["--enter-bootloader", "--force"],
            NOT_FOUND,
            &["/dev/hidraw3", "/dev/hidraw9"],
        ),
    );
}
