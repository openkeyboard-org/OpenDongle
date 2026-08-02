//! Cross-platform USB-HID maintenance tool for the OpenDongle dongle (WCH
//! CH592F / CH570). Reads device/bond/fault status over the vendor IAP
//! interface and reboots the dongle into the OpenBoot bootloader for
//! updates (`--enter-bootloader`); the flashing itself happens in OpenBoot
//! over OBP via the `openboot` CLI.
//!
//! Safe by default: with no action it displays device information.

mod bond;
mod fault;
mod flows;
mod hex;
mod iap;
mod status;

use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{anyhow, Result};
use bond::show_bond_info;
use clap::Parser;
use fault::show_fault_info;
use hidapi::HidApi;

use flows::{enter_bootloader, probe};
use hex::load_firmware;
use iap::{op_version, IapDevice, DEFAULT_PID, DEFAULT_VID, IAP_INTERFACE};
use status::{read_status, show_status};

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
        --enter-bootloader reboots the dongle into OpenBoot (1209:0001), \
        then flash with the openboot CLI."
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

    /// Reboot the dongle into the OpenBoot bootloader (it re-enumerates as
    /// VID:PID 1209:0001; flash there with the openboot CLI)
    #[arg(long)]
    enter_bootloader: bool,

    /// The update image you intend to flash next. Checked against the
    /// device family (ODG2 header vs GetDevInfo) BEFORE the reboot, so a
    /// wrong-family image is refused while the app is still running.
    #[arg(long, value_name = "FILE", requires = "enter_bootloader")]
    image: Option<PathBuf>,

    /// With --enter-bootloader and no --image: reboot without the
    /// family guard (you take wrong-image risk into your own hands)
    #[arg(long, requires = "enter_bootloader")]
    force: bool,
}

impl Cli {
    fn show_info(&self) -> bool {
        self.info || !self.enter_bootloader
    }
}

fn run(cli: &Cli) -> Result<ExitCode> {
    let api = HidApi::new()?;

    let dev = match IapDevice::open(&api, cli.vid, cli.pid, cli.interface, cli.hidraw.as_deref()) {
        Ok(d) => d,
        Err(e) => {
            let msg = e.to_string();
            if msg.contains("no HID device") {
                if cli.enter_bootloader && openboot_present(&api) {
                    // Already sitting in OpenBoot, so there is nothing to
                    // reboot. The family guard CANNOT run here: it compares the
                    // image against the family the *application* reports, and
                    // the application is not running. Say so rather than
                    // implying the image was checked.
                    println!(
                        "no {:04X}:{:04X} app, but an OpenBoot bootloader (1209:0001) \
                         is on the bus — already in the bootloader",
                        cli.vid, cli.pid
                    );
                    if cli.image.is_some() && !cli.force {
                        eprintln!(
                            "ERROR: the image family could NOT be verified — that check \
                             needs the running application, which is not present."
                        );
                        eprintln!(
                            "       Confirm the image matches this device yourself (the \
                             bootloader reports its chip family in `openboot probe`), \
                             then re-run with --force to acknowledge the guard was skipped."
                        );
                        return Ok(ExitCode::from(3));
                    }
                    println!("next: openboot flash --force <app.bin>  (family NOT verified)");
                    return Ok(ExitCode::SUCCESS);
                }
                eprintln!(
                    "ERROR: no hidraw node for VID=0x{:04X} PID=0x{:04X} interface={}",
                    cli.vid, cli.pid, cli.interface
                );
                eprintln!(
                    "       (tip: confirm the dongle is plugged in; check device \
                     permissions / udev rules)"
                );
            } else {
                eprintln!("ERROR: cannot open device: {msg}");
                eprintln!(
                    "       On Linux, run as root or add your user to the 'plugdev' \
                     group and reconnect the dongle."
                );
            }
            return Ok(ExitCode::from(1));
        }
    };
    println!(
        "device: {}  (VID=0x{:04X} PID=0x{:04X} if={})",
        dev.path, cli.vid, cli.pid, cli.interface
    );

    let device_info = probe(&dev)?;
    if cli.show_info() {
        let response =
            op_version(&dev)?.ok_or_else(|| anyhow!("Version: no response (timeout)"))?;
        let end = response
            .iter()
            .position(|&byte| byte == 0)
            .unwrap_or(response.len());
        let firmware_version = String::from_utf8_lossy(&response[..end]);
        let status = read_status(&dev)?;
        show_status(&status, &firmware_version);
        show_bond_info(&dev)?;
        show_fault_info(&dev)?;
        println!("tool:");
        println!("  version         {}", env!("CARGO_PKG_VERSION"));
        return Ok(ExitCode::SUCCESS);
    }

    // --enter-bootloader from here on.
    if let Some(ipath) = &cli.image {
        // Family guard: refuse a wrong-family image BEFORE the reboot, while
        // the app (which knows its family) is still the thing answering.
        let image = load_firmware(ipath)?;
        image.validate_for_device((device_info.chip_id & 0xff) as u8)?;
        println!(
            "\nimage {}: base=0x{base:08X}, {} bytes",
            ipath.display(),
            image.bytes.len(),
            base = image.base,
        );
        if let Some(id) = image.odg2 {
            println!(
                "  ODG2 family=0x{:02X} build=0x{:08X} crc=0x{:08X}",
                id.family, id.build_id, id.image_crc32
            );
        }
    } else if !cli.force {
        return Err(anyhow!(
            "refusing --enter-bootloader without a family check: pass \
             --image <the update .bin> (checked against the device family), \
             or --force to skip the guard"
        ));
    }

    enter_bootloader(&dev)?;
    drop(dev);

    // The device resets within ~a second (RF quiesce + EP6 drain). Success
    // is not mere disappearance — wait for the OpenBoot bootloader to
    // actually arrive on the bus. (With more than one dongle attached this
    // check is not path-precise; the bench wrapper additionally requires
    // exactly one 1209:0001 device before flashing.)
    let mut app_gone = false;
    let mut boot_here = false;
    for _ in 0..50 {
        std::thread::sleep(std::time::Duration::from_millis(200));
        let api = HidApi::new()?;
        if !app_gone {
            app_gone = !api.device_list().any(|d| {
                d.vendor_id() == cli.vid
                    && d.product_id() == cli.pid
                    && d.interface_number() == cli.interface
            });
        }
        if openboot_present(&api) {
            boot_here = true;
            break;
        }
    }
    if boot_here {
        println!("OpenBoot bootloader (1209:0001) is on the bus");
        match &cli.image {
            Some(p) => println!("next: openboot flash --force {}", p.display()),
            None => println!("next: openboot flash --force <app.bin>"),
        }
        Ok(ExitCode::SUCCESS)
    } else if app_gone {
        eprintln!(
            "WARNING: app left the bus but no OpenBoot bootloader appeared within 10 s"
        );
        Ok(ExitCode::from(2))
    } else {
        eprintln!("WARNING: device still present 10 s after EnterBootloader was accepted");
        Ok(ExitCode::from(2))
    }
}

/// OpenBoot's USB identity (pid.codes test PID; see OpenBoot PROTOCOL.md §12).
fn openboot_present(api: &HidApi) -> bool {
    api.device_list()
        .any(|d| d.vendor_id() == 0x1209 && d.product_id() == 0x0001)
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    match run(&cli) {
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
    fn force_requires_enter_bootloader() {
        // --force alone used to parse and then silently fall into the info
        // path, which reads as "the guard was skipped" when nothing was asked.
        assert!(Cli::try_parse_from(["opendongle", "--force"]).is_err());
        assert!(Cli::try_parse_from(["opendongle", "--enter-bootloader", "--force"]).is_ok());
    }

    #[test]
    fn enter_bootloader_cli_contract() {
        // --image requires --enter-bootloader...
        assert!(Cli::try_parse_from(["opendongle", "--image", "fw.bin"]).is_err());
        // ...and --info conflicts with --enter-bootloader.
        assert!(Cli::try_parse_from(["opendongle", "--info", "--enter-bootloader"]).is_err());
        let full = Cli::try_parse_from([
            "opendongle", "--enter-bootloader", "--image", "fw.bin",
        ])
        .unwrap();
        assert!(full.enter_bootloader);
        assert!(full.image.is_some());
    }

    #[test]
    fn help_identifies_tool_version_without_version_action() {
        let help = Cli::command().render_long_help().to_string();
        assert!(help.starts_with(concat!("opendongle ", env!("CARGO_PKG_VERSION"))));
        assert!(Cli::try_parse_from(["opendongle", "--version"]).is_err());
    }
}
