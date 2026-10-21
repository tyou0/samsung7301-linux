//! Wire protocol for the Samsung `04e8:7301` fingerprint sensor.
//!
//! # Provenance
//!
//! The constants and framing below were recovered by disassembling the
//! official Samsung Windows driver for this exact laptop: `fpc_wbdi.dll`,
//! version 3.23.0.15 (Fingerprint Cards AB). See `docs/REVERSE-ENGINEERING.md`
//! for how the driver was obtained.
//!
//! The sensor is an FPC part. Samsung ships it under vendor ID `04e8`, and the
//! driver's INF binds `USB\VID_04E8&PID_7301` and `PID_7305` to the same
//! "FPC Fingerprint Reader" stack.
//!
//! An earlier revision of this file borrowed the `04e8:7305` *control
//! transfer* protocol. That was wrong: the real driver uses **bulk** transfers
//! with a `40 c3` / `40 ca` header (see [`HEADER_REG`] and [`HEADER_BULK`]).
//!
//! Everything here is still derived from static analysis, not a capture. The
//! command opcodes are certain (they come from the driver's own name table);
//! the per-command *type byte* and the reply trailer are not.

use std::time::Duration;

use anyhow::{Context, Result};
use rusb::DeviceHandle;

/// Samsung USB vendor ID.
pub const VID: u16 = 0x04e8;
/// The sensor this crate targets.
pub const PID_7301: u16 = 0x7301;
/// Sibling sensor sharing the same driver (for reference/experiments).
pub const PID_7305: u16 = 0x7305;

/// Bulk OUT endpoint (host -> device).
pub const EP_OUT: u8 = 0x01;
/// Bulk IN endpoint (device -> host).
pub const EP_IN: u8 = 0x82;
/// Interrupt IN endpoint (status / doorbell).
pub const EP_INTR: u8 = 0x83;

pub const CMD_TIMEOUT: Duration = Duration::from_millis(3_000);
pub const IMG_TIMEOUT: Duration = Duration::from_millis(3_000);

/// Largest payload the driver moves in a single bulk transfer.
pub const MAX_XFER: usize = 0x6400;

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------
//
// Every command is a 4-byte header followed by a payload:
//
//   register/value form:  40 c3 00 00 <payload...>
//   bulk data form:       40 ca <len_lo> <len_hi> <type_byte> <payload...>

/// Register/command header (`40 c3 00 00` little-endian `0x0000c340`).
pub const HEADER_REG: [u8; 4] = [0x40, 0xc3, 0x00, 0x00];

/// Bulk data header prefix (`40 ca`, then a 16-bit length).
pub const HEADER_BULK: [u8; 2] = [0x40, 0xca];

/// Build a `40 ca` bulk packet: header, little-endian length, type byte,
/// payload. `type_byte` is the command-specific selector the driver supplies;
/// the exact values are not yet known (pass `0` when probing).
pub fn bulk_packet(type_byte: u8, payload: &[u8]) -> Vec<u8> {
    let len = payload.len() as u16;
    let mut pkt = Vec::with_capacity(4 + 1 + payload.len());
    pkt.extend_from_slice(&HEADER_BULK);
    pkt.extend_from_slice(&len.to_le_bytes());
    pkt.push(type_byte);
    pkt.extend_from_slice(payload);
    pkt
}

/// Build a `40 c3` register packet.
pub fn reg_packet(payload: &[u8]) -> Vec<u8> {
    let mut pkt = Vec::with_capacity(4 + payload.len());
    pkt.extend_from_slice(&HEADER_REG);
    pkt.extend_from_slice(payload);
    pkt
}

// ---------------------------------------------------------------------------
// Command opcodes
// ---------------------------------------------------------------------------
//
// Recovered from the byte->name dispatcher at fpc_wbdi.dll+0x18000fb5c.

pub const CMD_NAVDATA: u8 = 0x10;
pub const CMD_READ_INTERRUPT_WITH_CLEAR: u8 = 0x1c;
pub const CMD_FINGER_PRESENT_QUERY: u8 = 0x20;
pub const CMD_WAIT_FOR_FINGER_PRESENT: u8 = 0x24;
pub const CMD_ACTIVATE_SLEEP_MODE: u8 = 0x28;
pub const CMD_ACTIVATE_DEEP_SLEEP_MODE: u8 = 0x2c;
pub const CMD_ACTIVATENAV: u8 = 0x30;
pub const CMD_ACTIVATE_IDLE_MODE: u8 = 0x34;
pub const CMD_CLKBIST: u8 = 0x3c;
pub const CMD_IMGSMPLSETUP: u8 = 0x4c;
pub const CMD_IMGCAPSIZE: u8 = 0x54;
pub const CMD_IMAGESETUP: u8 = 0x5c;
pub const CMD_IMAGERD: u8 = 0x64;
pub const CMD_SAMPLEDLY: u8 = 0x68;
pub const CMD_PXLRSTDLY: u8 = 0x6c;
pub const CMD_TSTCOLPATTERNEN: u8 = 0x78;
pub const CMD_CLKBISTRESULT: u8 = 0x7c;
pub const CMD_OFFSETDAC: u8 = 0x80;
pub const CMD_FNGRDRIVECONF: u8 = 0x8c;
pub const CMD_FNGRDRIVEDLY: u8 = 0x90;
pub const CMD_OSCTRIM: u8 = 0x94;
pub const CMD_ADCWEIGHTTBL: u8 = 0x98;
pub const CMD_ADCSETUP: u8 = 0x9c;
pub const CMD_ADCSHIFTGAIN: u8 = 0xa0;
pub const CMD_PXLCTRL: u8 = 0xa8;
pub const CMD_AFDCTRL: u8 = 0xac;
pub const CMD_CAPTURE_IMAGE: u8 = 0xb4;
pub const CMD_READ_IMAGE_DATA: u8 = 0xb8;
pub const CMD_FNGRPRESENTSTATUS: u8 = 0xc8;
pub const CMD_FNGRDETTHRESH: u8 = 0xd8;
pub const CMD_FNGRDETCNTR: u8 = 0xdc;
pub const CMD_FNGRDOWNSHRT: u8 = 0xe0;
pub const CMD_FNGRDOWNLONG: u8 = 0xe4;
pub const CMD_FNGRDOWNHOLD: u8 = 0xe8;
pub const CMD_FNGRAREACTRL: u8 = 0xec;
pub const CMD_AFDCALIBRATION: u8 = 0xf0;
pub const CMD_AFDSTATUS: u8 = 0xf4;
pub const CMD_SOFT_RESET: u8 = 0xf8;
pub const CMD_HWID: u8 = 0xfc;

/// Human-readable name for a command opcode, mirroring the driver's table.
pub fn op_name(op: u8) -> &'static str {
    match op {
        CMD_NAVDATA => "navdata",
        CMD_READ_INTERRUPT_WITH_CLEAR => "read_interrupt_with_clear",
        CMD_FINGER_PRESENT_QUERY => "finger_present_query",
        CMD_WAIT_FOR_FINGER_PRESENT => "wait_for_finger_present",
        CMD_ACTIVATE_SLEEP_MODE => "activate_sleep_mode",
        CMD_ACTIVATE_DEEP_SLEEP_MODE => "activate_deep_sleep_mode",
        CMD_ACTIVATENAV => "activatenav",
        CMD_ACTIVATE_IDLE_MODE => "activate_idle_mode",
        CMD_CLKBIST => "clkbist",
        CMD_IMGSMPLSETUP => "imgsmplsetup",
        CMD_IMGCAPSIZE => "imgcapsize",
        CMD_IMAGESETUP => "imagesetup",
        CMD_IMAGERD => "imagerd",
        CMD_SAMPLEDLY => "sampledly",
        CMD_PXLRSTDLY => "pxlrstdly",
        CMD_TSTCOLPATTERNEN => "tstcolpatternen",
        CMD_CLKBISTRESULT => "clkbistresult",
        CMD_OFFSETDAC => "offsetdac",
        CMD_FNGRDRIVECONF => "fngrdriveconf",
        CMD_FNGRDRIVEDLY => "fngrdrivedly",
        CMD_OSCTRIM => "osctrim",
        CMD_ADCWEIGHTTBL => "adcweighttbl",
        CMD_ADCSETUP => "adcsetup",
        CMD_ADCSHIFTGAIN => "adcshiftgain",
        CMD_PXLCTRL => "pxlctrl",
        CMD_AFDCTRL => "afdctrl",
        CMD_CAPTURE_IMAGE => "capture_image",
        CMD_READ_IMAGE_DATA => "read_image_data",
        CMD_FNGRPRESENTSTATUS => "fngrpresentstatus",
        CMD_FNGRDETTHRESH => "fngrdetthresh",
        CMD_FNGRDETCNTR => "fngrdetcntr",
        CMD_FNGRDOWNSHRT => "fngrdownshrt",
        CMD_FNGRDOWNLONG => "fngrdownlong",
        CMD_FNGRDOWNHOLD => "fngrdownhold",
        CMD_FNGRAREACTRL => "fngrareactrl",
        CMD_AFDCALIBRATION => "afdcalibration",
        CMD_AFDSTATUS => "afdstatus",
        CMD_SOFT_RESET => "soft_reset",
        CMD_HWID => "hwid",
        _ => "unknown",
    }
}

// ---------------------------------------------------------------------------
// Register configuration
// ---------------------------------------------------------------------------

/// Register configuration words for chip ID `0x4ec` (FPC1022), from
/// `fpc_wbdi.dll+0x1800071d8`. Little-endian 16-bit words; `0x0100` terminates
/// a `40 c3` packet.
pub const REG_CONFIG_4EC: &[u16] = &[
    0x004c, 0x0004, 0x00ff, 0x00ff, 0x0011, 0x0100,
    0x006c, 0x0004, 0x0100, 0x0068, 0x0013, 0x0008, 0x0008, 0x0008, 0x0008,
    0x0014, 0x0014, 0x0014, 0x0014, 0x0100,
    0x0080, 0x0082, 0x0015, 0x0015, 0x0015, 0x0015, 0x0009, 0x0009, 0x0009,
    0x0009, 0x0100,
    0x0098, 0x0003, 0x00eb, 0x003f, 0x00b8, 0x000a, 0x0007, 0x0011, 0x0022,
    0x006a, 0x005d, 0x0100,
    0x008c, 0x0000, 0x0004,
];

/// Split [`REG_CONFIG_4EC`] into `40 c3` register packets, flushing at each
/// `0x0100` terminator.
pub fn reg_packets(words: &[u16]) -> Vec<Vec<u8>> {
    let mut packets = Vec::new();
    let mut current = Vec::new();
    for &w in words {
        if w == 0x0100 {
            if !current.is_empty() {
                packets.push(reg_packet(&current));
                current.clear();
            }
        } else {
            current.extend_from_slice(&w.to_le_bytes());
        }
    }
    if !current.is_empty() {
        packets.push(reg_packet(&current));
    }
    packets
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

/// Write one framed packet to the bulk OUT endpoint.
pub fn write_packet(handle: &DeviceHandle<rusb::Context>, packet: &[u8]) -> Result<usize> {
    handle
        .write_bulk(EP_OUT, packet, CMD_TIMEOUT)
        .with_context(|| format!("bulk OUT {} bytes", packet.len()))
}

/// Read a reply from the bulk IN endpoint.
pub fn read_reply(handle: &DeviceHandle<rusb::Context>, max: usize) -> Result<Vec<u8>> {
    let mut buf = vec![0u8; max.min(MAX_XFER + 2)];
    let n = handle
        .read_bulk(EP_IN, &mut buf, IMG_TIMEOUT)
        .context("bulk IN reply")?;
    buf.truncate(n);
    Ok(buf)
}

/// Decode the finger-present state from a status reply.
///
/// The driver's `fngrpresentstatus` (0xc8) reply is not fully traced, so this
/// is best-effort and reports raw bytes when the layout is unexpected.
pub fn decode_status(resp: &[u8]) -> String {
    match resp.first() {
        Some(&0x00) => "ok / no finger".to_string(),
        Some(&b) if b & 0x80 != 0 => format!("finger present (0x{b:02x})"),
        Some(&b) => format!("state 0x{b:02x}"),
        None => "empty reply".to_string(),
    }
}
