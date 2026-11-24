// Copyright 2026 Eric Molitor (EMulator)
// SPDX-License-Identifier: Apache-2.0

//! A scripted dongle for tests: a [`ReportLink`] whose replies come from a
//! closure, so every flow can be driven and its exchanges inspected without
//! hardware.

use std::cell::RefCell;
use std::collections::VecDeque;
use std::rc::Rc;

use anyhow::Result;

use crate::iap::{IapDevice, ReportLink};

/// What the device does with one request: `(cmd, body)` in, the reply out.
/// `None` is silence, which the host sees as a timeout.
pub type Responder = Box<dyn FnMut(u8, &[u8]) -> Option<Vec<u8>>>;

struct Inner {
    respond: Responder,
    pending: VecDeque<Vec<u8>>,
    log: Vec<(u8, Vec<u8>)>,
}

/// Cloning shares the device, so a test can keep a handle to read the log
/// after the [`IapDevice`] built on it is gone.
#[derive(Clone)]
pub struct ScriptedLink(Rc<RefCell<Inner>>);

impl ScriptedLink {
    pub fn new(respond: impl FnMut(u8, &[u8]) -> Option<Vec<u8>> + 'static) -> ScriptedLink {
        ScriptedLink(Rc::new(RefCell::new(Inner {
            respond: Box::new(respond),
            pending: VecDeque::new(),
            log: Vec::new(),
        })))
    }

    /// An [`IapDevice`] talking to this script.
    pub fn device(&self) -> IapDevice {
        IapDevice::with_link(Box::new(self.clone()), "scripted")
    }

    /// Every request the host sent, as `(cmd, body)`, in order.
    pub fn log(&self) -> Vec<(u8, Vec<u8>)> {
        self.0.borrow().log.clone()
    }

    /// The command bytes alone.
    pub fn commands(&self) -> Vec<u8> {
        self.0.borrow().log.iter().map(|(cmd, _)| *cmd).collect()
    }
}

impl ReportLink for ScriptedLink {
    fn write_report(&self, report: &[u8]) -> Result<()> {
        // [report id][cmd][len][body...][checksum]
        let cmd = report[1];
        let len = usize::from(report[2]);
        let body = report[3..3 + len].to_vec();
        let mut inner = self.0.borrow_mut();
        inner.log.push((cmd, body.clone()));
        if let Some(mut reply) = (inner.respond)(cmd, &body) {
            // A real reply is the 64-byte report payload.
            if reply.len() < 64 {
                reply.resize(64, 0);
            }
            inner.pending.push_back(reply);
        }
        Ok(())
    }

    fn read_report(&self, buf: &mut [u8], _timeout_ms: i32) -> Result<usize> {
        match self.0.borrow_mut().pending.pop_front() {
            Some(reply) => {
                buf[..reply.len()].copy_from_slice(&reply);
                Ok(reply.len())
            }
            None => Ok(0),
        }
    }
}
