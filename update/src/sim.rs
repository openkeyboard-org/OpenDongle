// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! A simulated dongle for the transaction tests: one device that is either
//! running its application or sitting in OpenBoot, with a bus, a clock and
//! the two protocols in front of it — and a set of faults to inject.
//!
//! It models the behaviour the transaction relies on, as read from the
//! bootloader's source: a HELLO opens a session that suspends the idle
//! auto-boot; the active slot is never the one written; COMMIT swaps the
//! slots and answers a repeat of the same request with OK; BOOT mid-update
//! returns to the previous application; an idle bootloader goes back to the
//! application after ten seconds.

use std::cell::RefCell;
use std::collections::VecDeque;
use std::rc::Rc;
use std::time::{Duration, Instant};

use anyhow::{bail, Result};

use openboot::proto::consts::{
    OB_CMD_BOOT, OB_CMD_COMMIT, OB_CMD_CRC, OB_CMD_ERASE, OB_CMD_HELLO, OB_CMD_RESP_BIT,
    OB_CMD_WRITE, OB_DET_VERIFY_MISMATCH, OB_E_STATE, OB_E_VERIFY, OB_FAMILY_CH592,
    OB_FEAT_CRC_LIVE, OB_OK, OB_TRANSPORT_ID_USB,
};
use openboot::proto::Frame;
use openboot::transport::Transport;
use opendongle::handoff::{Bus, BusEntry, Clock, OB_USAGE, OB_USAGE_PAGE};
use opendongle::iap::IapDevice;
use opendongle::scripted::ScriptedLink;

use crate::transaction::{Bootloader, Devices};

pub const VID: u16 = 0x0C45;
pub const PID: u16 = 0xFEFE;
pub const APP_PATH: &str = "app";
pub const BOOT_PATH: &str = "boot";
/// The dongle's UID as its application reports it, and another dongle's.
pub const UID: [u8; 8] = [0xB2, 0x59, 0x21, 0x62, 0x32, 0xDC, 0x05, 0x98];
pub const OTHER_UID: [u8; 8] = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0xCC, 0x99];
/// What its bootloader reports: on CH592, an unrelated number.
const BOOT_UID: u64 = 0x0123_4567_89AB_CDEF;
const BASES: [u32; 2] = [0x2000, 0x39000];
const IDLE_TIMEOUT: Duration = Duration::from_secs(10);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Mode {
    App,
    Boot,
    /// Not on the bus at all.
    Gone,
}

#[derive(Clone, Copy, PartialEq, Eq, Default)]
pub enum CommitFault {
    #[default]
    None,
    /// The device says the image does not match.
    Mismatch,
    /// The commit lands; no reply to it ever arrives.
    ReplyLost,
    /// The request never reaches the device.
    NeverArrives,
    /// The commit lands, then the device stops answering anything — until it
    /// is power-cycled, a second later.
    LandsThenSilent,
}

#[derive(Clone, Copy, PartialEq, Eq, Default)]
pub enum BootFault {
    #[default]
    None,
    /// The device boots; its reply is lost.
    ReplyLost,
    /// The first BOOT never reaches the device.
    FirstNeverArrives,
}

#[derive(Default)]
pub struct Faults {
    pub refuse_reboot: bool,
    pub bootloader_already_present: bool,
    pub cannot_open_bootloader: bool,
    pub wrong_serial: bool,
    /// The nth WRITE (1-based) is refused by the device.
    pub fail_write: Option<usize>,
    pub commit: CommitFault,
    pub live_crc_wrong: bool,
    pub boot: BootFault,
    /// The bootloader's boot-time image check rejects the new slot.
    pub new_image_fails_boot_check: bool,
    /// After the next boot, nothing enumerates.
    pub application_never_returns: bool,
    /// After the next boot, the application answering is another dongle's.
    pub another_dongle_returns: bool,
    /// From the nth HELLO on (1-based), the device handles it — the session
    /// opens — and its reply is lost.
    pub hello_replies_lost_from: Option<usize>,
    /// The nth attempt to open the bootloader (1-based) fails; the rest work.
    pub bootloader_open_fails_on: Option<usize>,
    /// After a reset the application takes this long to enumerate.
    pub returns_after: Option<Duration>,
    /// A second dongle is attached throughout, running its application.
    pub bystander: bool,
}

pub struct World {
    pub mode: Mode,
    /// The build installed in each slot.
    pub slots: [Option<u32>; 2],
    pub active: usize,
    pub faults: Faults,
    /// Every OBP command the bootloader received, in order.
    pub obp: Vec<u8>,

    session: bool,
    written: Vec<u8>,
    /// The slot a COMMIT landed in this power cycle, and its request.
    committed: Option<(usize, Vec<u8>)>,
    idle: Duration,
    silent: Option<Duration>,
    writes: usize,
    boots: usize,
    /// How many times the application has started.
    starts: usize,
    hellos: usize,
    /// Attempts to open the bootloader, whether or not they worked.
    opens: usize,
    /// How long until the application, having reset, is on the bus again.
    returning: Option<Duration>,
}

pub type Shared = Rc<RefCell<World>>;

impl World {
    /// A dongle running `build` from slot A.
    pub fn running(build: u32) -> Shared {
        Rc::new(RefCell::new(World {
            mode: Mode::App,
            slots: [Some(build), None],
            active: 0,
            faults: Faults::default(),
            obp: Vec::new(),
            session: false,
            written: Vec::new(),
            committed: None,
            idle: Duration::ZERO,
            silent: None,
            writes: 0,
            boots: 0,
            starts: 1,
            hellos: 0,
            opens: 0,
            returning: None,
        }))
    }

    pub fn running_build(&self) -> Option<u32> {
        self.slots[self.active]
    }

    pub fn sent(&self, cmd: u8) -> usize {
        self.obp.iter().filter(|&&c| c == cmd).count()
    }

    fn write_slot(&self) -> usize {
        match &self.committed {
            Some((slot, _)) => 1 - slot,
            None => 1 - self.active,
        }
    }

    fn reported_active(&self) -> usize {
        match &self.committed {
            Some((slot, _)) => *slot,
            None => self.active,
        }
    }

    /// A reset: the boot decision picks a slot, and the application starts.
    fn reset(&mut self) {
        if let Some((slot, _)) = self.committed.take() {
            if !self.faults.new_image_fails_boot_check {
                self.active = slot;
            }
        }
        self.session = false;
        self.written.clear();
        self.idle = Duration::ZERO;
        self.silent = None;
        self.starts += 1;
        self.mode = if self.faults.application_never_returns {
            Mode::Gone
        } else if let Some(delay) = self.faults.returns_after {
            self.returning = Some(delay);
            Mode::Gone
        } else {
            Mode::App
        };
    }

    fn tick(&mut self, d: Duration) {
        if let Some(left) = self.returning {
            if d >= left {
                self.returning = None;
                self.mode = Mode::App;
            } else {
                self.returning = Some(left - d);
            }
            return;
        }
        if self.mode != Mode::Boot {
            return;
        }
        if let Some(silent) = &mut self.silent {
            *silent += d;
            if *silent >= Duration::from_secs(1) {
                self.reset(); // the user pulled it out and put it back
            }
            return;
        }
        if !self.session {
            self.idle += d;
            if self.idle >= IDLE_TIMEOUT {
                self.reset();
            }
        }
    }

    fn hello_payload(&self) -> Vec<u8> {
        let write = self.write_slot();
        let mut p = vec![OB_OK, 0, 2, 0x92];
        p.extend_from_slice(&0x000Bu16.to_le_bytes()); // bootloader version
        p.push(OB_FAMILY_CH592);
        p.push(OB_TRANSPORT_ID_USB);
        p.extend_from_slice(&0x2000u32.to_le_bytes()); // app start
        p.extend_from_slice(&0x70000u32.to_le_bytes()); // app end
        p.extend_from_slice(&4096u32.to_le_bytes()); // erase block
        p.extend_from_slice(&256u16.to_le_bytes()); // write page
        p.push(4); // write align
        p.push(48); // max write data
        p.extend_from_slice(&OB_FEAT_CRC_LIVE.to_le_bytes());
        p.extend_from_slice(&BOOT_UID.to_le_bytes());
        p.push(2); // slot count
        p.push(self.reported_active() as u8);
        p.push(write as u8);
        p.push(0);
        p.extend_from_slice(&BASES[write].to_le_bytes());
        p.extend_from_slice(&0x36000u32.to_le_bytes());
        p
    }

    /// One OBP request in, its response payload out (`None`: no reply).
    fn obp(&mut self, cmd: u8, payload: &[u8]) -> Option<Vec<u8>> {
        if self.mode != Mode::Boot || self.silent.is_some() {
            return None;
        }
        self.obp.push(cmd);
        if cmd != OB_CMD_HELLO && !self.session {
            return Some(vec![OB_E_STATE, 0]);
        }
        match cmd {
            OB_CMD_HELLO => {
                self.session = true;
                self.written.clear();
                self.hellos += 1;
                if self
                    .faults
                    .hello_replies_lost_from
                    .is_some_and(|nth| self.hellos >= nth)
                {
                    return None;
                }
                Some(self.hello_payload())
            }
            OB_CMD_ERASE => Some(vec![OB_OK]),
            OB_CMD_WRITE => {
                self.writes += 1;
                if self.faults.fail_write == Some(self.writes) {
                    return Some(vec![0x08, 0x00]); // a flash error
                }
                self.written.extend_from_slice(&payload[4..]);
                Some(vec![OB_OK])
            }
            OB_CMD_COMMIT => {
                // A repeat of the request that landed is acknowledged as-is.
                if let Some((_, request)) = &self.committed {
                    let repeat = request == payload;
                    return match self.faults.commit {
                        CommitFault::ReplyLost => None,
                        _ if repeat => Some(vec![OB_OK]),
                        _ => Some(vec![OB_E_VERIFY, OB_DET_VERIFY_MISMATCH]),
                    };
                }
                match self.faults.commit {
                    CommitFault::Mismatch => {
                        return Some(vec![OB_E_VERIFY, OB_DET_VERIFY_MISMATCH])
                    }
                    CommitFault::NeverArrives => {
                        self.obp.pop();
                        return None;
                    }
                    _ => {}
                }
                let len = u32::from_le_bytes(payload[..4].try_into().unwrap()) as usize;
                let crc = u32::from_le_bytes(payload[4..8].try_into().unwrap());
                if self.written.len() != len || crc32fast::hash(&self.written) != crc {
                    return Some(vec![OB_E_VERIFY, OB_DET_VERIFY_MISMATCH]);
                }
                let build = u32::from_le_bytes(self.written[0x34..0x38].try_into().unwrap());
                let slot = self.write_slot();
                self.slots[slot] = Some(build);
                self.committed = Some((slot, payload.to_vec()));
                match self.faults.commit {
                    CommitFault::ReplyLost => None,
                    CommitFault::LandsThenSilent => {
                        self.silent = Some(Duration::ZERO);
                        None
                    }
                    _ => Some(vec![OB_OK]),
                }
            }
            OB_CMD_CRC => {
                let mut crc = crc32fast::hash(&self.written);
                if self.faults.live_crc_wrong {
                    crc ^= 0xFFFF;
                }
                let mut p = vec![OB_OK];
                p.extend_from_slice(&crc.to_le_bytes());
                Some(p)
            }
            OB_CMD_BOOT => {
                self.boots += 1;
                if self.faults.boot == BootFault::FirstNeverArrives && self.boots == 1 {
                    self.obp.pop();
                    return None;
                }
                self.reset();
                match self.faults.boot {
                    BootFault::ReplyLost => None,
                    _ => Some(vec![OB_OK]),
                }
            }
            _ => Some(vec![0x02, 0]),
        }
    }

    /// One IAP request to the application.
    fn iap(&mut self, cmd: u8, body: &[u8]) -> Option<Vec<u8>> {
        if self.mode != Mode::App {
            return None;
        }
        Some(match cmd {
            0x5A => vec![0xA5],
            0x84 if body.first() == Some(&1) => {
                vec![0x04, 0x0A, 0, 0, 0, 0, 0, 0, 0x92, 0, 0, 0]
            }
            0x84 => vec![0x0F, 0x01, 0, 0],
            0x85 if self.faults.refuse_reboot => vec![0x0F, 0x01, 0xE2, 0],
            0x85 => {
                self.mode = Mode::Boot;
                self.idle = Duration::ZERO;
                self.writes = 0;
                self.boots = 0;
                vec![0x0F, 0x01, 0, 0]
            }
            0x91 => {
                let other = self.faults.another_dongle_returns && self.starts > 1;
                status_reply(if other { &OTHER_UID } else { &UID }, self.running_build()?)
            }
            _ => return None,
        })
    }
}

/// An application's answer to the status command.
fn status_reply(uid: &[u8; 8], build: u32) -> Vec<u8> {
    let mut p = [0u8; 32];
    p[0] = 1; // schema
    p[1] = 0x92; // family
    p[2] = 3; // connected
    p[4] = 0x0F;
    p[5] = 8;
    p[6] = 1;
    p[8..16].copy_from_slice(uid);
    p[16..20].copy_from_slice(&build.to_le_bytes());
    p[20..24].copy_from_slice(&0x1000u32.to_le_bytes());
    let mut reply = vec![0x91, 32];
    reply.extend_from_slice(&p);
    reply
}

/// The other dongle's application: it answers the status read with its own
/// UID and nothing else, and is never the one being updated.
fn bystander() -> IapDevice {
    ScriptedLink::new(|cmd, _| (cmd == 0x91).then(|| status_reply(&OTHER_UID, 0x0BAD))).device()
}

/// The application's IAP interface.
pub fn application(world: &Shared) -> (ScriptedLink, IapDevice) {
    let world = world.clone();
    let link = ScriptedLink::new(move |cmd, body| world.borrow_mut().iap(cmd, body));
    let dev = link.device();
    (link, dev)
}

struct SimBootloader {
    world: Shared,
    rx: VecDeque<Vec<u8>>,
}

impl Transport for SimBootloader {
    fn send_frame(&mut self, frame: &[u8]) -> Result<()> {
        let req = Frame::decode(frame).expect("the host sent a frame that decodes");
        if let Some(payload) = self.world.borrow_mut().obp(req.cmd, &req.payload) {
            self.rx
                .push_back(Frame::new(req.cmd | OB_CMD_RESP_BIT, req.seq, payload).encode());
        }
        Ok(())
    }

    fn recv_frame(&mut self, _deadline: Instant) -> Result<Option<Vec<u8>>> {
        Ok(self.rx.pop_front())
    }
}

pub struct SimDevices(pub Shared);

impl Devices for SimDevices {
    fn open_bootloader(&mut self, _vid: u16, _pid: u16, path: &str) -> Result<Bootloader> {
        let mut world = self.0.borrow_mut();
        world.opens += 1;
        if world.faults.cannot_open_bootloader
            || world.faults.bootloader_open_fails_on == Some(world.opens)
        {
            bail!("access denied");
        }
        if world.mode != Mode::Boot || path != BOOT_PATH {
            bail!("no bootloader at {path}");
        }
        let serial = if world.faults.wrong_serial {
            "FFFFFFFFFFFFFFFF".to_string()
        } else {
            format!("{BOOT_UID:016X}")
        };
        Ok(Bootloader {
            transport: Box::new(SimBootloader {
                world: self.0.clone(),
                rx: VecDeque::new(),
            }),
            serial: Some(serial),
        })
    }

    fn open_applications(&mut self, _vid: u16, _pid: u16, _interface: i32) -> Vec<IapDevice> {
        // The bystander first: that is the order that used to end the wait.
        let mut found = Vec::new();
        if self.0.borrow().faults.bystander {
            found.push(bystander());
        }
        if self.0.borrow().mode == Mode::App {
            found.push(application(&self.0).1);
        }
        found
    }
}

pub struct SimBus(pub Shared);

impl Bus for SimBus {
    fn scan(&mut self) -> Result<Vec<BusEntry>> {
        let world = self.0.borrow();
        let entry = |path: &str, interface, usage_page, usage| BusEntry {
            path: path.to_string(),
            vid: VID,
            pid: PID,
            interface,
            usage_page,
            usage,
        };
        let mut bus = Vec::new();
        match world.mode {
            Mode::App => bus.push(entry(APP_PATH, 4, 0xFF60, 0x0061)),
            Mode::Boot => bus.push(entry(BOOT_PATH, 0, OB_USAGE_PAGE, OB_USAGE)),
            Mode::Gone => {}
        }
        if world.faults.bootloader_already_present {
            bus.push(entry("someone-else", 0, OB_USAGE_PAGE, OB_USAGE));
        }
        if world.faults.bystander {
            bus.push(entry("app-other", 4, 0xFF60, 0x0061));
        }
        Ok(bus)
    }
}

/// Time passes only when slept, and the simulated dongle ages with it.
pub struct SimClock {
    world: Shared,
    now: Instant,
}

impl SimClock {
    pub fn new(world: &Shared) -> SimClock {
        SimClock {
            world: world.clone(),
            now: Instant::now(),
        }
    }
}

impl Clock for SimClock {
    fn now(&mut self) -> Instant {
        self.now
    }

    fn sleep(&mut self, d: Duration) {
        self.now += d;
        self.world.borrow_mut().tick(d);
    }
}
