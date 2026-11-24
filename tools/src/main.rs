// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! Cross-platform USB-HID maintenance tool for the OpenDongle dongle (WCH
//! CH592F / CH570). Reads device/bond/fault status over the vendor IAP
//! interface and reboots the dongle into the OpenBoot bootloader for
//! updates (`--enter-bootloader`); the flashing itself happens in OpenBoot
//! over OBP via the `openboot` CLI.
//!
//! Safe by default: with no action it displays device information.

#[cfg(test)]
mod golden_tests;

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::{anyhow, Result};
use clap::Parser;
use hidapi::HidApi;

use opendongle::console::{self, flash_next_step, Console, HandoffContext};
use opendongle::handoff::{self, HidBus, SystemClock, Target, OB_USAGE, OB_USAGE_PAGE};
use opendongle::iap::{
    path_identity, require_path_identity, IapDevice, DEFAULT_PID, DEFAULT_VID, IAP_INTERFACE,
};
use opendongle::{bond, crypt, errln, outln};
/// Parse an integer the way Python's `int(s, 0)` does: 0x/0o/0b prefixes select
/// the radix, otherwise decimal.
fn parse_int_auto(s: &str) -> Result<u64, String> {
    let t = s.trim();
    let r = if let Some(h) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        u64::from_str_radix(h, 16)
    } else if let Some(o) = t.strip_prefix("0o").or_else(|| t.strip_prefix("0O")) {
        u64::from_str_radix(o, 8)
    } else if let Some(b) = t.strip_prefix("0b").or_else(|| t.strip_prefix("0B")) {
        u64::from_str_radix(b, 2)
    } else {
        t.parse::<u64>()
    };
    r.map_err(|e| format!("invalid integer '{s}': {e}"))
}

fn parse_u16_auto(s: &str) -> Result<u16, String> {
    let v = parse_int_auto(s)?;
    u16::try_from(v).map_err(|_| format!("value out of range for u16: {s}"))
}

#[derive(Parser)]
#[command(
    name = "opendongle",
    version = env!("CARGO_PKG_VERSION"),
    disable_version_flag = true,
    help_template = "{name} {version}\n{about-with-newline}\n{usage-heading} {usage}\n\n{all-args}{after-help}",
    about = "OpenDongle CH570/CH592 maintenance over USB HID IAP.",
    long_about = "OpenDongle CH570/CH592 maintenance over USB HID IAP.\n\n\
        Safe by default: without an action, displays device, firmware, and \
        bond information. Updates happen in the OpenBoot bootloader: \
        --enter-bootloader reboots the dongle into OpenBoot, which enumerates \
        under this same VID:PID and is told apart by its HID usage page \
        0xFF00. Flash there with `openboot --vid 0x0C45 --pid 0xFEFE`."
)]
struct Cli {
    /// USB VID (default 0x0C45)
    #[arg(long, value_parser = parse_u16_auto, default_value_t = DEFAULT_VID)]
    vid: u16,

    /// USB PID (default 0xFEFE)
    #[arg(long, value_parser = parse_u16_auto, default_value_t = DEFAULT_PID)]
    pid: u16,

    /// USB interface number (default 4)
    #[arg(long, default_value_t = IAP_INTERFACE)]
    interface: i32,

    /// Explicit HID device path (skip discovery)
    #[arg(long, visible_alias = "path")]
    hidraw: Option<String>,

    /// Display device, firmware version, and persistent bond information
    #[arg(long, conflicts_with = "enter_bootloader")]
    info: bool,

    /// Read only the retained fault page (command 0x93). Unlike --info this
    /// does not arm or disarm a session: it issues a single unarmed exchange
    /// and leaves any existing session state exactly as it found it, so it
    /// is the readout to use on a unit whose state you are diagnosing.
    #[arg(long, conflicts_with_all = ["enter_bootloader", "info"])]
    fault: bool,

    /// Print one line of link state (connection, last RSSI) from a single
    /// unarmed Status (0x91) exchange; repeatable with --samples/--period-ms
    /// for watching state flicker without arming a session.
    #[arg(long, conflicts_with_all = ["enter_bootloader", "info", "fault"])]
    status: bool,

    /// Read the RF diagnostics pages (command 0x92: radio arm/event counters,
    /// executor state, protocol counters) with unarmed exchanges only;
    /// repeatable with --samples/--period-ms, which then also prints the
    /// per-second rate of every counter that changed.
    #[arg(long, conflicts_with_all = ["enter_bootloader", "info", "fault", "status"])]
    diag: bool,

    /// RF intervention ladder (command 0x94, arms a session): 1 = re-arm RX
    /// from task context, 2 = shut + vendor re-init + re-arm (CH570 only),
    /// 3 = shut the radio and leave it deaf (fault injection: the terminal
    /// camp's liveness watchdog must bring it back within its 200 ms tick),
    /// 40+N (N = 0..19) = on a live link, mask every interrupt for a nominal
    /// N poll slots so the polls coalesce into one gap of about N intervals
    /// (the hop repeat-correction regression check: 5 and 10 must not drop
    /// the link); 60+M and 70+M (M = 0..9) = a nominal 5- or 6-slot gap plus
    /// 3*M ticks, the remainder bands. Requested durations: the mask starts
    /// at an arbitrary phase within the current slot, so the realised gap is
    /// the request plus that phase.
    /// Diagnostic: run it on a dongle that is deaf to its keyboard, rung 1
    /// first; which rung restores the link says whether the software loop or
    /// the PHY was dead. The firmware refuses rungs 1-3 while a link is up
    /// and rungs 40-79 unless one is; every rung is refused while quiescing.
    #[arg(long, value_name = "RUNG", conflicts_with_all = ["enter_bootloader", "info", "fault", "status", "diag"])]
    rf_poke: Option<u8>,

    /// BENCH: install this 16-byte link key (32 hex characters) on the stored
    /// bond and activate link encryption (arms a session). The keyboard must
    /// have advertised encryption at pairing and must be given the same key.
    /// There is no on-air key establishment yet; do not use a real secret.
    #[arg(long, value_name = "HEX32", conflicts_with_all = ["enter_bootloader", "info", "fault", "status", "diag", "rf_poke"])]
    provision_link_key: Option<String>,

    /// Erase the stored bond (arms a session). Replug the dongle afterwards:
    /// it accepts no new pair until reset, then stays in pairing.
    #[arg(long, conflicts_with_all = ["enter_bootloader", "info", "fault", "status", "diag", "rf_poke", "provision_link_key"])]
    clear_bond: bool,

    /// Print the link-encryption counters (command 0x98, product layout):
    /// frames verified and frames dropped, by reason. Read-only, unarmed.
    #[arg(long, conflicts_with_all = ["enter_bootloader", "info", "fault", "status", "diag", "rf_poke", "provision_link_key", "clear_bond"])]
    crypt_diag: bool,

    /// Give the dongle 32 bytes of entropy for its next link-key exchange
    /// (command 0x9C). With no value the bytes come from this computer's
    /// random number generator; a value is 64 hex characters. The dongle has
    /// no hardware random source, so without a seed its key rests on radio
    /// timing alone. The seed is mixed into that, never used in its place,
    /// and is lost when the dongle is unplugged: send it after plugging the
    /// dongle in and before pairing.
    #[arg(long, value_name = "HEX64", num_args = 0..=1, default_missing_value = "", conflicts_with_all = ["enter_bootloader", "info", "fault", "status", "diag", "rf_poke", "provision_link_key", "clear_bond", "crypt_diag", "kex_bench"])]
    kex_seed: Option<String>,

    /// BENCH: time one X25519 scalar multiplication on the device (command
    /// 0x99, KEX_BENCH firmware only). The radio is starved while it runs.
    #[arg(long, conflicts_with_all = ["enter_bootloader", "info", "fault", "status", "diag", "rf_poke", "provision_link_key", "clear_bond", "crypt_diag"])]
    kex_bench: bool,

    /// With --status or --diag: number of samples (default 1, at least 1)
    #[arg(long, default_value_t = 1, value_parser = clap::value_parser!(u32).range(1..))]
    samples: u32,

    /// With --status or --diag: period between samples in milliseconds (default 200)
    #[arg(long, default_value_t = 200)]
    period_ms: u64,

    /// Reboot the dongle into the OpenBoot bootloader (it re-enumerates under
    /// the same VID:PID on HID usage page 0xFF00; flash there with the
    /// openboot CLI, passing --vid/--pid to match this device)
    #[arg(long)]
    enter_bootloader: bool,

    /// The update image you intend to flash next. Checked against the
    /// device family (ODG2 header vs GetDevInfo) BEFORE the reboot, so a
    /// wrong-family image is refused while the app is still running. Read
    /// host-side for that guard only -- it is never flashed; the artifact
    /// OpenBoot flashes is the .obb bundle (see --bundle).
    #[arg(long, value_name = "FILE", requires = "enter_bootloader")]
    image: Option<PathBuf>,

    /// The .obb update bundle to flash once the bootloader is up. Not read
    /// by this tool: it only names the artifact in the printed `openboot
    /// flash` next-step so that line can be copied and run as-is. A/B
    /// updates need the bundle -- OpenBoot alternates its write target, and
    /// a bare .bin only matches one slot by luck (firmware/BOOT.md,
    /// "Updating: use the bundle").
    #[arg(long, value_name = "FILE", requires = "enter_bootloader")]
    bundle: Option<PathBuf>,

    /// With --enter-bootloader and no --image: reboot without the
    /// family guard (you take wrong-image risk into your own hands)
    #[arg(long, requires = "enter_bootloader")]
    force: bool,
}

impl Cli {
    fn show_info(&self) -> bool {
        self.info
            || (!self.enter_bootloader
                && !self.fault
                && !self.status
                && !self.diag
                && self.rf_poke.is_none()
                && self.provision_link_key.is_none()
                && !self.clear_bond
                && !self.crypt_diag
                && !self.kex_bench
                && self.kex_seed.is_none())
    }

    /// --samples/--period-ms only mean something to the two samplers.
    fn sampler_args_valid(&self) -> bool {
        (self.samples == 1 && self.period_ms == 200) || self.status || self.diag
    }
}

/// `--image` takes the flat slot-A `.bin` (or `.hex`) the family guard reads;
/// an `.obb` bundle is a different container that load_firmware would
/// misparse as a flat image based at 0x2000 and then family-check garbage.
fn image_arg_is_bundle(path: &Path) -> bool {
    path.extension()
        .is_some_and(|e| e.eq_ignore_ascii_case("obb"))
}

/// What `device_commands` leaves for `run` to do.
enum Step {
    /// The command is finished; exit with this status.
    Exit(u8),
    /// `--enter-bootloader` passed its guards: reboot and watch the bus.
    EnterBootloader,
}

/// Every command that needs only the opened device: the readouts, the armed
/// operations, and the checks that come before a reboot into the bootloader.
fn device_commands(cli: &Cli, dev: &IapDevice, con: &mut Console) -> Result<Step> {
    outln!(
        con,
        "device: {}  (VID=0x{:04X} PID=0x{:04X} if={})",
        dev.path,
        cli.vid,
        cli.pid,
        cli.interface
    );

    if cli.fault {
        // Deliberately before the probe: probe arms and disarms a session, and
        // the whole point of --fault is to read without touching one.
        console::fault_info(dev, con)?;
        return Ok(Step::Exit(0));
    }
    if !cli.sampler_args_valid() {
        return Err(anyhow!(
            "--samples/--period-ms apply only to --status or --diag"
        ));
    }
    if let Some(text) = cli.kex_seed.as_deref() {
        let (seed, source) = if text.is_empty() {
            (
                crypt::random_seed()?,
                "this computer's random number generator",
            )
        } else {
            (crypt::parse_kex_seed(text)?, "the command line")
        };
        console::kex_seed(dev, &seed, source, con)?;
        return Ok(Step::Exit(0));
    }
    if cli.kex_bench {
        console::kex_bench(dev, con)?;
        return Ok(Step::Exit(0));
    }
    if cli.crypt_diag {
        console::crypt_diag(dev, con)?;
        return Ok(Step::Exit(0));
    }
    if cli.clear_bond {
        console::clear_bond(dev, con)?;
        return Ok(Step::Exit(0));
    }
    if let Some(text) = cli.provision_link_key.as_deref() {
        let key = bond::parse_link_key(text)?;
        console::provision_link_key(dev, &key, con)?;
        return Ok(Step::Exit(0));
    }
    if let Some(rung) = cli.rf_poke {
        let done = console::rf_poke(dev, rung, con)?;
        return Ok(Step::Exit(if done { 0 } else { 1 }));
    }
    if cli.diag {
        console::diag_sampler(dev, cli.samples, cli.period_ms, con)?;
        return Ok(Step::Exit(0));
    }
    if cli.status {
        console::status_sampler(dev, cli.samples, cli.period_ms, con)?;
        return Ok(Step::Exit(0));
    }

    let device_info = console::probe(dev, con)?;
    if cli.show_info() {
        console::info(dev, con)?;
        return Ok(Step::Exit(0));
    }

    // --enter-bootloader from here on.
    if let Some(ipath) = &cli.image {
        console::check_image(ipath, &device_info, con)?;
    } else if !cli.force {
        return Err(anyhow!(
            "refusing --enter-bootloader without a family check: pass \
             --image <the update .bin> (checked against the device family), \
             or --force to skip the guard"
        ));
    }
    Ok(Step::EnterBootloader)
}

/// The application's IAP interface could not be opened. Say why, as far as
/// the bus shows it, and return the exit status. `boots` is every OpenBoot
/// interface on this VID:PID: with `--enter-bootloader`, exactly one means the
/// dongle is already where it was being sent.
fn report_open_failure(cli: &Cli, msg: &str, boots: &[String], con: &mut Console) -> u8 {
    if msg.contains("no HID device") {
        if cli.enter_bootloader && boots.len() > 1 {
            errln!(
                con,
                "ERROR: {} OpenBoot bootloaders are on the bus; cannot say \
                 which one this is meant to be.",
                boots.len()
            );
            errln!(
                con,
                "       openboot refuses a multi-match too. One at a time."
            );
            return 2;
        }
        if cli.enter_bootloader && boots.len() == 1 {
            // Already sitting in OpenBoot, so there is nothing to
            // reboot. The family guard CANNOT run here: it compares the
            // image against the family the *application* reports, and
            // the application is not running. Say so rather than
            // implying the image was checked.
            outln!(
                con,
                "no {:04X}:{:04X} app interface, but an OpenBoot bootloader is \
                 on the bus at {} (same VID:PID, HID usage page {:04X} usage \
                 {:02X}) — already in the bootloader",
                cli.vid,
                cli.pid,
                boots[0],
                OB_USAGE_PAGE,
                OB_USAGE
            );
            if cli.image.is_some() && !cli.force {
                errln!(
                    con,
                    "ERROR: the image family could NOT be verified — that check \
                     needs the running application, which is not present."
                );
                errln!(
                    con,
                    "       Confirm the image matches this device yourself (the \
                     bootloader reports its chip family in `openboot probe`), \
                     then re-run with --force to acknowledge the guard was skipped."
                );
                return 3;
            }
            // The flags are not optional: openboot defaults to its
            // generic 1209:0001 and would not find this product. Name
            // the bundle when one was given, so the printed line can be
            // copied and run as-is rather than silently flashing a
            // placeholder the user has to remember to substitute.
            outln!(
                con,
                "{}",
                flash_next_step(
                    cli.vid,
                    cli.pid,
                    cli.bundle.as_deref(),
                    "  (family NOT verified)"
                )
            );
            return 0;
        }
        errln!(
            con,
            "ERROR: no hidraw node for VID=0x{:04X} PID=0x{:04X} interface={}",
            cli.vid,
            cli.pid,
            cli.interface
        );
        errln!(
            con,
            "       (tip: confirm the dongle is plugged in; check device \
             permissions / udev rules)"
        );
    } else {
        errln!(con, "ERROR: cannot open device: {msg}");
        errln!(
            con,
            "       On Linux, run as root or add your user to the 'plugdev' \
             group and reconnect the dongle."
        );
    }
    1
}

fn run(cli: &Cli, con: &mut Console) -> Result<ExitCode> {
    if let Some(ipath) = &cli.image {
        if image_arg_is_bundle(ipath) {
            return Err(anyhow!(
                "--image {} is an .obb bundle; the family guard needs the flat \
                 slot-A .bin (or .hex). Pass that as --image and the bundle as \
                 --bundle -- the bundle is what OpenBoot flashes afterwards",
                ipath.display()
            ));
        }
    }

    // The mirror of the guard above. --bundle is never read here; it is only
    // interpolated into the copy-and-run `openboot flash` line, so a .bin
    // passed here is printed as if it were flashable. Copying that line hands
    // OpenBoot a slot-A-based flat image while it alternates its write target,
    // which is right for one slot by luck and wrong for the other.
    if let Some(bpath) = &cli.bundle {
        if !image_arg_is_bundle(bpath) {
            return Err(anyhow!(
                "--bundle {} is not an .obb bundle; OpenBoot needs the bundle so \
                 it can pick the variant for the slot it is actually writing. \
                 Pass the flat slot-A .bin as --image instead",
                bpath.display()
            ));
        }
    }

    let api = HidApi::new()?;

    // --hidraw opens whatever path it names, with no check that the node
    // carries cli.vid/pid/interface (IapDevice::open). --fault and --status
    // send a vendor report without a prior probe, so before anything is even
    // opened confirm from enumeration alone that the path is the interface the
    // selectors describe; a mismatch is a refusal, not a note. Discovered
    // devices are matched on those selectors already and are not affected.
    if cli.fault
        || cli.status
        || cli.diag
        || cli.rf_poke.is_some()
        || cli.provision_link_key.is_some()
        || cli.clear_bond
        || cli.crypt_diag
        || cli.kex_bench
        || cli.kex_seed.is_some()
    {
        if let Some(path) = cli.hidraw.as_deref() {
            require_path_identity(&api, path, cli.vid, cli.pid, cli.interface)?;
        }
    }

    let dev = match IapDevice::open(&api, cli.vid, cli.pid, cli.interface, cli.hidraw.as_deref()) {
        Ok(d) => d,
        Err(e) => {
            let boots = handoff::bootloader_paths(&handoff::entries(&api), cli.vid, cli.pid);
            let code = report_open_failure(cli, &e.to_string(), &boots, con);
            return Ok(ExitCode::from(code));
        }
    };
    match device_commands(cli, &dev, con)? {
        Step::Exit(code) => return Ok(ExitCode::from(code)),
        Step::EnterBootloader => {}
    }

    // Which interface is being rebooted. The watch tracks it by path, so the
    // path has to be one a user chose — or the only one there was to choose.
    let target = match cli.hidraw.as_deref() {
        None => {
            // Discovery opened the first match. With one dongle that is the
            // dongle; with several it is an arbitrary one, and rebooting an
            // arbitrary one leaves a bootloader on the bus that the flash
            // which follows would then be aimed at.
            let apps = handoff::application_paths(
                &handoff::entries(&HidApi::new()?),
                cli.vid,
                cli.pid,
                cli.interface,
            );
            if apps.len() > 1 {
                errln!(
                    con,
                    "ERROR: {} dongles are running their application on {:04X}:{:04X}; \
                     cannot say which one to reboot.",
                    apps.len(),
                    cli.vid,
                    cli.pid
                );
                errln!(
                    con,
                    "       Unplug the others, or name this one with --hidraw. None was rebooted."
                );
                return Ok(ExitCode::from(2));
            }
            Target {
                path: dev.path.clone(),
                vid: cli.vid,
                pid: cli.pid,
            }
        }
        // --hidraw opens a path directly, with no check that it carries
        // cli.vid, cli.pid or cli.interface (iap.rs). The watch is phrased in
        // the target's identity, so an explicit path pointing somewhere else
        // would have us watching one device and reporting on another. Resolve
        // the opened path's real identity and track THAT; refuse if the path
        // is not in the device list, since then nothing can be tracked at all.
        Some(path) => match path_identity(&HidApi::new()?, path) {
            Some(id) => {
                if (id.vid, id.pid, id.interface) != (cli.vid, cli.pid, cli.interface) {
                    outln!(
                        con,
                        "note: --hidraw {} is {:04X}:{:04X} if={}, not the \
                         {:04X}:{:04X} if={} selectors; tracking the opened device",
                        path,
                        id.vid,
                        id.pid,
                        id.interface,
                        cli.vid,
                        cli.pid,
                        cli.interface
                    );
                }
                Target {
                    path: path.to_string(),
                    vid: id.vid,
                    pid: id.pid,
                }
            }
            None => {
                errln!(
                    con,
                    "ERROR: --hidraw {path} is not in the HID device list, so the \
                     reboot cannot be observed."
                );
                return Ok(ExitCode::from(1));
            }
        },
    };

    // Success is not mere disappearance: the addressed application has to
    // leave AND exactly one new bootloader has to arrive and stay. Neither
    // this tool nor the openboot CLI can pin a physical device across the
    // re-enumeration, so that is the strongest available check. See
    // tools/README.md on the single-device requirement.
    let outcome =
        handoff::enter_and_watch(dev, &target, &mut HidBus, &mut SystemClock, &mut || {
            console::reboot_accepted(con)
        })?;
    let cx = HandoffContext {
        vid: cli.vid,
        pid: cli.pid,
        interface: cli.interface,
        bundle: cli.bundle.as_deref(),
    };
    Ok(ExitCode::from(console::report_handoff(&outcome, &cx, con)))
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    let mut con = Console::stdio();
    match run(&cli, &mut con) {
        Ok(code) => code,
        Err(e) => {
            eprintln!("ERROR: {e}");
            ExitCode::from(1)
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use clap::CommandFactory;

    #[test]
    fn int_parser_radix() {
        assert_eq!(parse_int_auto("0x0C45").unwrap(), 0x0C45);
        assert_eq!(parse_int_auto("0xFEFE").unwrap(), 0xFEFE);
        assert_eq!(parse_int_auto("4").unwrap(), 4);
        assert_eq!(parse_int_auto("0o17").unwrap(), 0o17);
        assert_eq!(parse_int_auto("0b1010").unwrap(), 0b1010);
        assert!(parse_int_auto("nope").is_err());
        assert_eq!(parse_u16_auto("0x0C45").unwrap(), 0x0C45);
        assert!(parse_u16_auto("0x1FFFF").is_err()); // > u16
    }

    #[test]
    fn default_action_is_full_info() {
        let default = Cli::try_parse_from(["opendongle"]).unwrap();
        assert!(default.show_info());

        let explicit = Cli::try_parse_from(["opendongle", "--info"]).unwrap();
        assert!(explicit.show_info());

        let enter = Cli::try_parse_from(["opendongle", "--enter-bootloader"]).unwrap();
        assert!(!enter.show_info());
    }

    #[test]
    fn fault_readout_is_its_own_action() {
        // --fault must not fall through to the armed --info flow, and it is
        // exclusive with the actions that arm a session or reboot.
        let fault = Cli::try_parse_from(["opendongle", "--fault"]).unwrap();
        assert!(fault.fault);
        assert!(!fault.show_info());
        assert!(Cli::try_parse_from(["opendongle", "--fault", "--info"]).is_err());
        assert!(Cli::try_parse_from(["opendongle", "--fault", "--enter-bootloader"]).is_err());
    }

    #[test]
    fn status_sampler_is_its_own_action() {
        // --status is the unarmed sampler: not the --info flow, exclusive
        // with the actions that arm or reboot, and the sampling knobs only
        // make sense alongside it.
        let status = Cli::try_parse_from(["opendongle", "--status"]).unwrap();
        assert!(status.status);
        assert!(!status.show_info());
        assert_eq!((status.samples, status.period_ms), (1, 200));
        let sampled = Cli::try_parse_from([
            "opendongle",
            "--status",
            "--samples",
            "50",
            "--period-ms",
            "100",
        ])
        .unwrap();
        assert_eq!((sampled.samples, sampled.period_ms), (50, 100));
        // The sampling knobs parse on their own (they are shared by --status
        // and --diag) but are refused at run time without a sampler action.
        let stray = Cli::try_parse_from(["opendongle", "--samples", "5"]).unwrap();
        assert!(!stray.sampler_args_valid());
        let stray = Cli::try_parse_from(["opendongle", "--period-ms", "5"]).unwrap();
        assert!(!stray.sampler_args_valid());
        assert!(Cli::try_parse_from(["opendongle", "--status", "--samples", "0"]).is_err());
        let diag = Cli::try_parse_from(["opendongle", "--diag", "--samples", "3"]).unwrap();
        assert!(diag.diag && diag.sampler_args_valid() && !diag.show_info());
        assert!(Cli::try_parse_from(["opendongle", "--diag", "--status"]).is_err());
        let poke = Cli::try_parse_from(["opendongle", "--rf-poke", "2"]).unwrap();
        assert_eq!(poke.rf_poke, Some(2));
        assert!(!poke.show_info());
        assert!(Cli::try_parse_from(["opendongle", "--rf-poke", "1", "--diag"]).is_err());
        assert!(Cli::try_parse_from(["opendongle", "--status", "--info"]).is_err());
        assert!(Cli::try_parse_from(["opendongle", "--status", "--fault"]).is_err());
        assert!(Cli::try_parse_from(["opendongle", "--status", "--enter-bootloader"]).is_err());
    }

    #[test]
    fn force_requires_enter_bootloader() {
        // --force alone used to parse and then silently fall into the info
        // path, which reads as "the guard was skipped" when nothing was asked.
        assert!(Cli::try_parse_from(["opendongle", "--force"]).is_err());
        assert!(Cli::try_parse_from(["opendongle", "--enter-bootloader", "--force"]).is_ok());
    }

    #[test]
    fn enter_bootloader_cli_contract() {
        // --image and --bundle require --enter-bootloader...
        assert!(Cli::try_parse_from(["opendongle", "--image", "fw.bin"]).is_err());
        assert!(Cli::try_parse_from(["opendongle", "--bundle", "fw.obb"]).is_err());
        // ...and --info conflicts with --enter-bootloader.
        assert!(Cli::try_parse_from(["opendongle", "--info", "--enter-bootloader"]).is_err());
        let full = Cli::try_parse_from([
            "opendongle",
            "--enter-bootloader",
            "--image",
            "fw.bin",
            "--bundle",
            "fw.obb",
        ])
        .unwrap();
        assert!(full.enter_bootloader);
        assert!(full.image.is_some());
        assert!(full.bundle.is_some());
    }

    #[test]
    fn obb_is_rejected_as_a_family_guard_image() {
        assert!(image_arg_is_bundle(Path::new("ch592-product.obb")));
        assert!(image_arg_is_bundle(Path::new("CH570.OBB")));
        assert!(!image_arg_is_bundle(Path::new("slot-a.bin")));
        assert!(!image_arg_is_bundle(Path::new("app.hex")));
        assert!(!image_arg_is_bundle(Path::new("no-extension")));
    }

    #[test]
    fn help_identifies_tool_version_without_version_action() {
        let help = Cli::command().render_long_help().to_string();
        assert!(help.starts_with(concat!("opendongle ", env!("CARGO_PKG_VERSION"))));
        assert!(Cli::try_parse_from(["opendongle", "--version"]).is_err());
    }
}
