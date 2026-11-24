// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! The built `opendongle` binary, run as a user would run it.
//!
//! Only what is refused before the HID bus is touched, so it needs no device
//! and cannot find one by accident. Everything that talks to a dongle is
//! pinned in `src/golden_tests.rs` against the scripted one. As there, a
//! deliberate change to the text is accepted with
//! `UPDATE_GOLDEN=1 cargo test` and reviewed as a diff of `tests/golden/`.

use std::path::PathBuf;
use std::process::{Command, Output};

fn opendongle(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_opendongle"))
        .args(args)
        .output()
        .expect("run opendongle")
}

/// stdout, stderr and the exit status as one transcript, with line endings
/// normalised so the same file serves every platform.
fn transcript(out: &Output) -> String {
    let text = |bytes: &[u8]| String::from_utf8_lossy(bytes).replace("\r\n", "\n");
    format!(
        "--- stdout ---\n{}--- stderr ---\n{}--- outcome ---\nexit {}\n",
        text(&out.stdout),
        text(&out.stderr),
        out.status
            .code()
            .expect("opendongle was not killed by a signal")
    )
}

fn check(name: &str, actual: String) {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("golden")
        .join(format!("{name}.txt"));
    if std::env::var_os("UPDATE_GOLDEN").is_some() {
        std::fs::write(&path, &actual).unwrap();
        return;
    }
    let expected = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| {
            panic!(
                "{}: {e}\nrun `UPDATE_GOLDEN=1 cargo test` to create it",
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

/// The family guard reads the flat slot-A image; a bundle handed to it would
/// be misparsed, so it is refused by its extension before anything else.
#[test]
fn a_bundle_is_refused_as_the_family_guard_image() {
    check(
        "cli_image_is_a_bundle",
        transcript(&opendongle(&["--enter-bootloader", "--image", "fw.obb"])),
    );
}

/// And the mirror: `--bundle` is printed into a copy-and-run line, so a .bin
/// there would be handed to OpenBoot as if it were flashable.
#[test]
fn a_bin_is_refused_as_the_bundle() {
    check(
        "cli_bundle_is_not_a_bundle",
        transcript(&opendongle(&[
            "--enter-bootloader",
            "--force",
            "--bundle",
            "fw.bin",
        ])),
    );
}

/// The text is clap's and names the binary as it was invoked, so only the
/// status is pinned.
#[test]
fn a_usage_error_exits_2() {
    let out = opendongle(&["--force"]);
    assert_eq!(out.status.code(), Some(2));
    assert!(out.stdout.is_empty());
    assert!(String::from_utf8_lossy(&out.stderr).contains("Usage"));
}

#[test]
fn help_exits_0_and_leads_with_the_version() {
    let out = opendongle(&["--help"]);
    assert_eq!(out.status.code(), Some(0));
    let help = String::from_utf8_lossy(&out.stdout);
    assert!(
        help.starts_with(concat!("opendongle ", env!("CARGO_PKG_VERSION"))),
        "{help}"
    );
}
