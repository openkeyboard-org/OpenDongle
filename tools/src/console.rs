// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! The CLI's presentation: one function per command, each of which runs the
//! library call and prints what the CLI prints for it.
//!
//! The rest of the crate prints nothing — it returns what it read, and the
//! decoders render blocks of text as lines. This module is the one place that
//! decides what reaches the terminal and in what order, including on the paths
//! where a command stops half-way. It lives in the library so that text can be
//! tested against a scripted device and reused by anything that wants the same
//! transcript.

use std::io::Write;
use std::path::Path;

use anyhow::Result;

use crate::bond::{self, render_bond, KeyActivation};
use crate::crypt;
use crate::fault::{read_fault, render_fault};
use crate::flows::{self, DevInfo};
use crate::handoff::{Handoff, OB_USAGE, OB_USAGE_PAGE};
use crate::hex::load_firmware;
use crate::iap::{hexsp, Armed, IapDevice, KEX_SEED_BYTES};
use crate::rfdiag;
use crate::status::{read_status, render_status};

/// Where the text goes: stdout and stderr as writers, so the exact output can
/// be captured and compared in tests.
pub struct Console {
    pub out: Box<dyn Write>,
    pub err: Box<dyn Write>,
}

impl Console {
    /// The process's stdout and stderr.
    pub fn stdio() -> Console {
        Console {
            out: Box::new(std::io::stdout()),
            err: Box::new(std::io::stderr()),
        }
    }
}

// A failed write to the console is ignored rather than propagated: losing a
// line of text must never abandon a device half-way through an exchange.
#[macro_export]
#[doc(hidden)]
macro_rules! outln {
    ($con:expr, $($arg:tt)*) => {{
        use std::io::Write as _;
        let _ = writeln!($con.out, $($arg)*);
    }};
}
#[macro_export]
#[doc(hidden)]
macro_rules! errln {
    ($con:expr, $($arg:tt)*) => {{
        use std::io::Write as _;
        let _ = writeln!($con.err, $($arg)*);
    }};
}

fn print_lines(con: &mut Console, lines: Vec<String>) {
    for line in lines {
        outln!(con, "{line}");
    }
}

/* --- readouts ----------------------------------------------------------- */

/// Handshake and GetDevInfo, printed as each lands — so a device that stops
/// answering part-way still shows what it did say.
pub fn probe(dev: &IapDevice, con: &mut Console) -> Result<DevInfo> {
    let reply = flows::handshake(dev)?;
    outln!(
        con,
        "  handshake       ack=0x{:02X}  raw={}",
        reply[0],
        hexsp(&reply, 8)
    );

    let Armed { result, disarm } = flows::read_devinfo(dev);
    let info = result?;
    outln!(
        con,
        "  GetDevInfo(1)   ack=0x{:02X}  raw={}",
        info.raw[0],
        hexsp(&info.raw, 14)
    );
    if info.is_pre_openboot() {
        // Pre-OpenBoot firmware still advertises its staging geometry here.
        outln!(
            con,
            "    staging_base  = 0x{:08X}  (pre-OpenBoot firmware)",
            info.reserved_geometry
        );
        outln!(con, "    block_size    = 0x{:04X}", info.reserved_block);
    }
    outln!(
        con,
        "    chip_id byte  = 0x{:02X}  ({})",
        info.family(),
        info.family_name()
    );
    // Leave the session in a known state: an unconfirmed disarm fails the
    // probe even though the identity was read.
    disarm?;
    Ok(info)
}

/// The rest of `--info`, after the probe: status, bond, health, tool version.
pub fn info(dev: &IapDevice, con: &mut Console) -> Result<()> {
    let firmware_version = flows::firmware_version(dev)?;
    let status = read_status(dev)?;
    print_lines(con, render_status(&status, &firmware_version));
    bond_info(dev, con)?;
    fault_info(dev, con)?;
    outln!(con, "tool:");
    outln!(con, "  version         {}", env!("CARGO_PKG_VERSION"));
    Ok(())
}

/// The bond block. Arms a session for the read and insists the disarm is
/// acknowledged before showing anything.
pub fn bond_info(dev: &IapDevice, con: &mut Console) -> Result<()> {
    let read = bond::read_bond(dev).strict()?;
    print_lines(con, render_bond(&read));
    Ok(())
}

/// The health block, from the retained fault page. Arms nothing.
pub fn fault_info(dev: &IapDevice, con: &mut Console) -> Result<()> {
    let fault = read_fault(dev)?;
    print_lines(con, render_fault(&fault));
    Ok(())
}

fn wall_clock() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs_f64())
        .unwrap_or(0.0)
}

/// `--status`: one line of link state per sample. 0x91 is unarmed, so this
/// samples without a probe.
pub fn status_sampler(
    dev: &IapDevice,
    samples: u32,
    period_ms: u64,
    con: &mut Console,
) -> Result<()> {
    for i in 0..samples {
        let status = read_status(dev)?;
        let now = wall_clock();
        outln!(
            con,
            "{now:.3} sample {i:>3}  connection={:<22} rssi={}",
            status.connection(),
            status.last_rssi()
        );
        if i + 1 < samples {
            std::thread::sleep(std::time::Duration::from_millis(period_ms));
        }
    }
    Ok(())
}

/// `--diag`: the RF diagnostics pages each sample and, from the second sample
/// on, the rate of every counter that changed. 0x92 is unarmed too.
pub fn diag_sampler(
    dev: &IapDevice,
    samples: u32,
    period_ms: u64,
    con: &mut Console,
) -> Result<()> {
    let mut prev: Option<(Vec<rfdiag::Page>, std::time::Instant)> = None;
    for i in 0..samples {
        let now = wall_clock();
        outln!(con, "--- {now:.3} diag sample {i}");
        let pages = rfdiag::read_all(dev)?;
        print_lines(con, rfdiag::render(&pages));
        let t = std::time::Instant::now();
        if let Some((p, pt)) = &prev {
            print_lines(
                con,
                rfdiag::rates(p, &pages, t.duration_since(*pt).as_secs_f64()),
            );
        }
        prev = Some((pages, t));
        if i + 1 < samples {
            std::thread::sleep(std::time::Duration::from_millis(period_ms));
        }
    }
    Ok(())
}

/* --- armed operations --------------------------------------------------- */

const DISARM_WARNING: &str =
    "warning: disarm not confirmed; replug the dongle to be sure the session is closed";

/// `--rf-poke`. Returns whether the rung ran; a refusal is printed, and is
/// the caller's to turn into an exit status.
pub fn rf_poke(dev: &IapDevice, rung: u8, con: &mut Console) -> Result<bool> {
    let poke = rfdiag::rf_poke(dev, rung).result?;
    outln!(
        con,
        "rf-poke rung {rung}: {} (rc=0x{:02X}), rf_state={}",
        poke.describe(),
        poke.rc,
        poke.rf_state
    );
    Ok(poke.done())
}

/// `--clear-bond`.
pub fn clear_bond(dev: &IapDevice, con: &mut Console) -> Result<()> {
    let Armed { result, disarm } = bond::clear_bond(dev);
    if disarm.is_err() {
        errln!(con, "{DISARM_WARNING}");
    }
    result?;
    outln!(con, "bond erased and verified; replug the dongle, then pair (it stays in pairing until a keyboard pairs)");
    Ok(())
}

/// `--provision-link-key`.
pub fn provision_link_key(dev: &IapDevice, key: &[u8; 16], con: &mut Console) -> Result<()> {
    let Armed { result, disarm } = bond::provision_link_key(dev, key);
    if disarm.is_err() {
        errln!(con, "{DISARM_WARNING}");
    }
    match result? {
        KeyActivation::Active => outln!(con, "link key written, verified and active"),
        KeyActivation::Deferred => outln!(
            con,
            "link key written and verified; activation deferred (reconnect or replug the dongle)"
        ),
    }
    Ok(())
}

/* --- link encryption ---------------------------------------------------- */

/// `--crypt-diag`.
pub fn crypt_diag(dev: &IapDevice, con: &mut Console) -> Result<()> {
    let diag = crypt::read_crypt_diag(dev)?;
    print_lines(con, crypt::render_crypt_diag(&diag));
    Ok(())
}

/// `--kex-bench`.
pub fn kex_bench(dev: &IapDevice, con: &mut Console) -> Result<()> {
    let bench = crypt::read_kex_bench(dev)?;
    print_lines(con, crypt::render_kex_bench(&bench));
    Ok(())
}

/// `--kex-seed`. `source` says where the bytes came from, for the operator.
pub fn kex_seed(
    dev: &IapDevice,
    seed: &[u8; KEX_SEED_BYTES],
    source: &str,
    con: &mut Console,
) -> Result<()> {
    crypt::send_kex_seed(dev, seed)?;
    outln!(con, "entropy seed accepted: 32 bytes from {source}");
    outln!(
        con,
        "it is used by the next key exchange and lost when the dongle is unplugged"
    );
    Ok(())
}

/* --- into the bootloader ------------------------------------------------ */

/// The family guard: refuse a wrong-family image BEFORE the reboot, while
/// the application (which knows its family) is still the thing answering.
pub fn check_image(path: &Path, device: &DevInfo, con: &mut Console) -> Result<()> {
    let image = load_firmware(path)?;
    image.validate_for_device(device.family())?;
    outln!(
        con,
        "\nimage {}: base=0x{base:08X}, {} bytes",
        path.display(),
        image.bytes.len(),
        base = image.base,
    );
    if let Some(id) = image.odg2 {
        outln!(
            con,
            "  ODG2 family=0x{:02X} build=0x{:08X} crc=0x{:08X}",
            id.family,
            id.build_id,
            id.image_crc32
        );
    }
    Ok(())
}

/// Single-quote a path for the printed hand-off line. The line exists to be
/// copied into a shell, so an unquoted path containing a space (or any other
/// shell metacharacter) would be split into the wrong arguments by whoever
/// pastes it. POSIX single quotes are literal apart from `'` itself, which is
/// escaped the usual `'\''` way.
pub fn shell_quote(s: &str) -> String {
    if !s.is_empty()
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"._-/=:+,@".contains(&b))
    {
        return s.to_string();
    }
    format!("'{}'", s.replace('\'', r"'\''"))
}

/// The copy-and-run `openboot flash` hand-off line. Always names the .obb
/// bundle -- never the family-guard `--image` .bin, which is host-side input
/// that OpenBoot must not be handed: it is slot-A-based, and OpenBoot
/// alternates its write target, so a bare .bin is only ever right for one
/// slot by luck (firmware/BOOT.md, "Updating: use the bundle").
pub fn flash_next_step(vid: u16, pid: u16, bundle: Option<&Path>, suffix: &str) -> String {
    let artifact = match bundle {
        Some(p) => shell_quote(&p.display().to_string()),
        None => "<chip>-product.obb".to_string(),
    };
    format!("next: openboot --vid 0x{vid:04X} --pid 0x{pid:04X} flash --force {artifact}{suffix}")
}

/// What the operator typed, as far as reporting a hand-off needs it.
pub struct HandoffContext<'a> {
    pub vid: u16,
    pub pid: u16,
    pub interface: i32,
    pub bundle: Option<&'a Path>,
}

/// The line printed once the dongle has agreed to reboot, before the wait.
pub fn reboot_accepted(con: &mut Console) {
    outln!(
        con,
        "  EnterBootloader accepted; device is resetting into OpenBoot"
    );
}

/// Say how a hand-off ended, and return the exit status for it: 0 only when
/// the addressed dongle is the one bootloader that arrived, 2 for every way
/// the hand-off could not be pinned to one device.
pub fn report_handoff(outcome: &Handoff, cx: &HandoffContext, con: &mut Console) -> u8 {
    match outcome {
        // Refuse rather than warn. Not because the follow-up flash could hit
        // the wrong unit - it could not; openboot narrows by HID usage page
        // and then bails outright when more than one interface matches, so
        // the worst case there is a clear error. The reason is that
        // proceeding is pointless and costly: we would reboot a working
        // dongle into a bootloader that openboot will then refuse to talk to,
        // and it idles back to the application ten seconds later. Failing
        // here leaves the device untouched.
        Handoff::BootloadersPresent { count } => {
            errln!(
                con,
                "ERROR: {count} OpenBoot bootloader(s) are already on the bus."
            );
            errln!(
                con,
                "       openboot selects by VID:PID plus HID usage page and refuses"
            );
            errln!(
                con,
                "       when more than one matches, so the flash would fail anyway."
            );
            errln!(
                con,
                "       Unplug the other dongle(s) first; this one is left running."
            );
            2
        }
        Handoff::Arrived { path } => {
            outln!(
                con,
                "OpenBoot bootloader is on the bus at {} ({:04X}:{:04X}, HID usage page \
                 {:04X} usage {:02X})",
                path,
                cx.vid,
                cx.pid,
                OB_USAGE_PAGE,
                OB_USAGE
            );
            outln!(con, "{}", flash_next_step(cx.vid, cx.pid, cx.bundle, ""));
            0
        }
        Handoff::MultipleArrived { count } => {
            errln!(
                con,
                "WARNING: {count} new OpenBoot devices appeared; cannot tell which is the \
                 one addressed."
            );
            errln!(
                con,
                "         Refusing the handoff: openboot narrows by HID usage page"
            );
            errln!(
                con,
                "         and rejects a multi-match, so the flash would fail anyway."
            );
            errln!(con, "         Attach one dongle at a time.");
            2
        }
        Handoff::AppNeverLeft => {
            errln!(
                con,
                "WARNING: an OpenBoot bootloader appeared, but the application interface \
                 we addressed ({:04X}:{:04X} if={}) never left the bus.",
                cx.vid,
                cx.pid,
                cx.interface
            );
            errln!(
                con,
                "         Refusing to report success: that is not the unit asked for."
            );
            2
        }
        Handoff::BootloaderLeft => {
            errln!(
                con,
                "WARNING: an OpenBoot bootloader appeared and then left the bus again."
            );
            errln!(
                con,
                "         It has most likely idled back into its application; there is"
            );
            errln!(
                con,
                "         nothing to flash. Run --enter-bootloader again."
            );
            2
        }
        Handoff::NoBootloader => {
            errln!(
                con,
                "WARNING: app left the bus but no OpenBoot bootloader appeared within 10 s"
            );
            2
        }
        Handoff::StillPresent => {
            errln!(
                con,
                "WARNING: device still present 10 s after EnterBootloader was accepted"
            );
            2
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The printed hand-off must name the .obb bundle -- BOOT.md's update flow
    // flashes the bundle, and a raw slot-A .bin handed to `openboot flash`
    // fails whenever slot B is the write target. Printing the family-guard
    // --image path here is the exact defect this line guards against.
    #[test]
    fn flash_next_step_names_the_bundle_never_the_image() {
        let with = flash_next_step(0x0C45, 0xFEFE, Some(Path::new("ch592-product.obb")), "");
        assert_eq!(
            with,
            "next: openboot --vid 0x0C45 --pid 0xFEFE flash --force ch592-product.obb"
        );
        let without = flash_next_step(0x0C45, 0xFEFE, None, "  (family NOT verified)");
        assert_eq!(
            without,
            "next: openboot --vid 0x0C45 --pid 0xFEFE flash --force \
             <chip>-product.obb  (family NOT verified)"
        );
        assert!(!without.contains(".bin"));
    }

    // --bundle is only ever printed into a copy-and-run shell line, so both
    // of these are about what a human ends up pasting.
    #[test]
    fn shell_quote_only_quotes_when_it_must() {
        assert_eq!(shell_quote("ch592-product.obb"), "ch592-product.obb");
        assert_eq!(
            shell_quote("build/ch570-product.obb"),
            "build/ch570-product.obb"
        );
        assert_eq!(shell_quote("my builds/a.obb"), "'my builds/a.obb'");
        assert_eq!(shell_quote("it's.obb"), r"'it'\''s.obb'");
        assert_eq!(shell_quote(""), "''");
    }

    #[test]
    fn flash_next_step_quotes_a_path_with_spaces() {
        let line = flash_next_step(0x0C45, 0xFEFE, Some(Path::new("my builds/a.obb")), "");
        assert!(
            line.contains("'my builds/a.obb'"),
            "unquoted path in: {line}"
        );
    }
}
