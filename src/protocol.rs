//! Wire protocol for the Samsung `04e8:7301` fingerprint sensor.
//!
//! # Provenance
//!
//! **Everything in this file is a hypothesis, not documentation.** The 7301
//! protocol has never been published. The framing and command bytes below are
//! copied from the sibling `04e8:7305` sensor, whose driver was
//! reverse-engineered from Windows USB captures:
//!
//!   https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/586
//!
//! The 7301 and 7305 expose an identical USB endpoint layout (bulk OUT EP1,
//! bulk IN EP2, interrupt IN EP3), which makes it plausible but *not proven*
//! that they share the transport. Treat every constant here as a probe
//! target; replace them with values observed in a capture of the real device.
//!
//! No code in this module is reachable from the default `info` mode. Commands
//! are only sent when the operator explicitly asks for them.

use std::time::Duration;

use anyhow::{Context, Result};
use rusb::DeviceHandle;

/// Samsung USB vendor ID.
pub const VID: u16 = 0x04e8;
/// The sensor this crate targets.
pub const PID_7301: u16 = 0x7301;
/// Sibling sensor the protocol is borrowed from (for reference/experiments).
pub const PID_7305: u16 = 0x7305;

/// Bulk OUT endpoint (host -> device).
pub const EP_OUT: u8 = 0x01;
/// Bulk IN endpoint (device -> host).
pub const EP_IN: u8 = 0x82;
/// Interrupt IN endpoint (status / doorbell).
pub const EP_INTR: u8 = 0x83;

pub const CMD_TIMEOUT: Duration = Duration::from_millis(2_000);
pub const IMG_TIMEOUT: Duration = Duration::from_millis(5_000);

/// Payload sent in the `0xc3` heartbeat control transfer. Identical in every
/// occurrence across the 7305 Windows capture, so it is very likely a
/// fixed transport header rather than device state.
pub const HEARTBEAT: [u8; 16] = [
    0xc0, 0xc6, 0x2d, 0x00, 0x08, 0x00, 0xff, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
];

// ---------------------------------------------------------------------------
// Command bytes (borrowed from samsung7305, see module docs)
// ---------------------------------------------------------------------------

pub const CMD_RESET: &[u8] = &[0xf8];
pub const CMD_STATUS: &[u8] = &[0x1c, 0x00];
pub const CMD_ARM: &[u8] = &[0x20];

// Status register (0x1c 0x00) response byte[1] values.
pub const STATUS_FINGER: u8 = 0x81;
pub const STATUS_IDLE: u8 = 0x80;
pub const STATUS_READY: u8 = 0x20;

/// The full 7305 init sequence. Kept here so the port is a matter of replaying
/// a known-good sequence, once a capture confirms or replaces it.
pub const INIT_SEQ: &[&[u8]] = &[
    &[0xf8],                                                 // reset
    &[0x1c, 0x00],                                           // status
    &[0x1c, 0x00],                                           // status
    &[0xfc, 0x00, 0x00],                                     // cfg1
    &[0x90, 0x34],                                           // cfg2
    &[0x68, 0x33, 0x33, 0x33, 0x33, 0x3f, 0x3f, 0x3f, 0x3f], // cfg3
    &[0x9c, 0x55, 0x40, 0x00, 0x2d, 0x24],                   // cfg4
    &[0x6c, 0x2b],                                           // cfg5
    &[0x8c, 0x12],                                           // cfg6
    &[0x5c, 0x0b],                                           // cfg7
    &[
        0x98, 0x03, 0xea, 0xbf, 0x68, 0x02, 0x09, 0x15, 0x2a, 0x7a, 0xde,
    ], // calib
    &[0xd8, 0x00, 0x01, 0x20, 0x20],                         // capparam
    &[0xe0, 0x32, 0x70, 0x22],                               // cfg8
    &[0xe4, 0x32, 0x88, 0x70],                               // cfg9
    &[0xe8, 0x32, 0x88],                                     // cfg10
    &[0xdc, 0x19, 0x01, 0x00, 0x0a],                         // cfg11
    &[0xa8, 0x0f, 0x0a],                                     // cfg12
    &[0xa0, 0x0a, 0x02],                                     // cfg13
    &[0x28],                                                 // init done
];

/// One command exchange, mirroring `bracketed_submit()` in samsung7305.c.
///
/// 1. `0xc3` vendor control OUT carrying the 16-byte heartbeat
/// 2. `0xca` vendor control OUT priming `prime_len` bytes of IN data
/// 3. bulk OUT the command bytes
/// 4. bulk IN `prime_len` bytes of response
/// 5. optional `0xda` vendor control IN "doorbell" (2 bytes)
pub fn exchange(
    handle: &DeviceHandle<rusb::Context>,
    cmd: &[u8],
    prime_len: u16,
    doorbell: bool,
) -> Result<Vec<u8>> {
    // 0x40 = host->device | vendor | recipient=device
    handle
        .write_control(0x40, 0xc3, 0x0000, 0x0000, &HEARTBEAT, CMD_TIMEOUT)
        .context("heartbeat control transfer (0xc3)")?;

    handle
        .write_control(0x40, 0xca, 0x0003, prime_len, &[], CMD_TIMEOUT)
        .context("prime control transfer (0xca)")?;

    handle
        .write_bulk(EP_OUT, cmd, CMD_TIMEOUT)
        .with_context(|| format!("bulk OUT command {cmd:02x?}"))?;

    let mut buf = vec![0u8; prime_len as usize];
    let n = handle
        .read_bulk(EP_IN, &mut buf, CMD_TIMEOUT)
        .with_context(|| format!("bulk IN after command {cmd:02x?}"))?;
    buf.truncate(n);

    if doorbell {
        let mut bell = [0u8; 2];
        // 0xc0 = device->host | vendor | recipient=device
        match handle.read_control(0xc0, 0xda, 0x0007, 0x0000, &mut bell, CMD_TIMEOUT) {
            Ok(n) => buf.extend_from_slice(&bell[..n]),
            // A missing doorbell is common on variants; not fatal for a probe.
            Err(e) => eprintln!("  (doorbell 0xda failed: {e})"),
        }
    }

    Ok(buf)
}

/// Decode the one-byte state field of a status response.
pub fn decode_status(resp: &[u8]) -> &'static str {
    match resp.get(1) {
        Some(&STATUS_FINGER) => "finger present (0x81)",
        Some(&STATUS_IDLE) => "idle (0x80)",
        Some(&STATUS_READY) => "ready (0x20)",
        Some(other) => {
            let _ = other;
            "unknown state"
        }
        None => "short/empty response",
    }
}
