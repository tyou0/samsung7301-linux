//! Wire protocol for the Samsung `04e8:7301` fingerprint sensor.
//!
//! # Provenance
//!
//! Everything here was recovered by disassembling the official Samsung Windows
//! driver for this exact laptop: `fpc_wbdi.dll`, version 3.23.0.15
//! (Fingerprint Cards AB). See `docs/REVERSE-ENGINEERING.md` for how the driver
//! was obtained and how each constant was derived.
//!
//! The sensor is an FPC part. Samsung ships it under vendor ID `04e8`, and the
//! driver's INF binds `USB\VID_04E8&PID_7301` and `PID_7305` to the same
//! "FPC Fingerprint Reader" stack.
//!
//! An earlier revision borrowed a *control-transfer* protocol from another FPC
//! part. That was wrong. The real driver uses **bulk** transfers exclusively,
//! with two packet forms ([`reg_packet`] and [`bulk_packet`]), and every
//! command is issued as a register write followed by a bulk write.
//!
//! # Bring-up
//!
//! The device does not answer commands until it has been taken out of its
//! default state. The driver's first transfer is a **single mode byte**
//! (`0x20`, `0x34` or `0x98`); it then waits for `0xc0` in the reply. See
//! [`bootstrap`].

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

/// Largest payload the driver moves in a single bulk transfer (`0x6400`).
pub const MAX_XFER: usize = 0x6400;
/// Staging-buffer size the driver allocates (`0x6402`, i.e. [`MAX_XFER`] + 2).
pub const STAGE_LEN: usize = 0x6402;

/// Magic value the device returns when a mode byte is accepted.
/// Magic value the device returns when a mode byte is accepted (observed in
/// the mode-ack path; kept for reference).
pub const MODE_ACK: u8 = 0xc0;

/// Chip ID of the FPC1022, the part this laptop carries.
pub const CHIP_ID_FPC1022: u32 = 0x4ec;

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------
//
// Two transports are in play, recovered from distinct helpers in
// `fpc_wbdi.dll`:
//
//   * the COMMAND channel is USB *control* transfers — vendor requests
//     `bRequest = 0xda` (read) and `0xdb` (write). See [`ctrl_write`] /
//     [`ctrl_read`].
//   * the DATA channel is *bulk* transfers on the OUT/IN pipes, carrying the
//     `40 c3` / `40 ca` frames built by `hw_usb_write` (`+0x18000ad1c`).
//
// The earlier revision treated the bulk frames as the command channel; that was
// wrong. See `docs/REVERSE-ENGINEERING.md` §6.

/// Control-request code for a **read** (device -> host) on the command channel.
/// Recovered from `fpc_wbdi.dll+0x180008e3c`.
pub const CREQ_READ: u8 = 0xda;
/// Control-request code for a **write** (host -> device) on the command channel.
/// Recovered from `fpc_wbdi.dll+0x180008eac`.
pub const CREQ_WRITE: u8 = 0xdb;
/// `bmRequestType` for a vendor, host-to-device request.
pub const BM_WRITE: u8 = 0x40;
/// `bmRequestType` for a vendor, device-to-host request.
pub const BM_READ: u8 = 0xc0;

/// Register/command header (`40 c3 00 00`, little-endian `0x0000c340`).
pub const HEADER_REG: [u8; 4] = [0x40, 0xc3, 0x00, 0x00];

/// Bulk data header prefix (`40 ca`).
pub const HEADER_BULK: [u8; 2] = [0x40, 0xca];

/// Type byte the driver puts after the bulk header. Recovered from the header
/// dword `0x0003ca40` in `hw_usb_bulk_write`; it is `0x03`, not command
/// specific.
pub const BULK_TYPE: u8 = 0x03;

/// Build a `40 ca` bulk packet: header, little-endian length, type byte,
/// payload.
pub fn bulk_packet(payload: &[u8]) -> Vec<u8> {
    let len = payload.len() as u16;
    let mut pkt = Vec::with_capacity(4 + 1 + payload.len());
    pkt.extend_from_slice(&HEADER_BULK);
    pkt.extend_from_slice(&len.to_le_bytes());
    pkt.push(BULK_TYPE);
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

/// The constant register frame the driver prefixes every bulk command with.
///
/// In `fpc_wbdi.dll+0x180008494` the header dword is `0x0000c340` and the
/// following word is written from `r15w`, which is zeroed on entry. So the
/// register packet is always exactly this, independent of the command.
pub const REG_FRAME: [u8; 6] = [0x40, 0xc3, 0x00, 0x00, 0x00, 0x00];

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
// Bring-up
// ---------------------------------------------------------------------------
//
// Recovered from `fpc_wbdi.dll+0x180008eac` (control write), `+0x180008e3c`
// (control read), `+0x180007c84` (chip config) and `+0x1800073e8` (status
// poll).

/// Control-request `wValue`s the driver writes during chip bring-up, from the
/// call sites listed in `docs/REVERSE-ENGINEERING.md` §6.3.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InitCmd {
    pub w_value: u16,
    pub w_index: u16,
}

/// Chip-init write used for chip ID `0x474`.
pub const INIT_474: InitCmd = InitCmd {
    w_value: 0x000b,
    w_index: 0,
};
/// Second chip-init write used for chip ID `0x474`.
pub const INIT_474_IDX1: InitCmd = InitCmd {
    w_value: 0x000b,
    w_index: 1,
};
/// Re-init writes observed without a chip gate.
pub const REINIT_0: InitCmd = InitCmd {
    w_value: 0x0006,
    w_index: 0,
};
pub const REINIT_1: InitCmd = InitCmd {
    w_value: 0x0006,
    w_index: 1,
};

/// Init command for FPC1022 (0x4ec) on this platform, inferred from live probe.
pub const INIT_FPC1022_0: InitCmd = InitCmd {
    w_value: 0x0006,
    w_index: 0,
};
pub const INIT_FPC1022_1: InitCmd = InitCmd {
    w_value: 0x0006,
    w_index: 1,
};

/// Control-request `wValue` the driver reads the status register with.
pub const STATUS_REG_WVALUE: u16 = 0x00d4;

/// Size of the status register read (`c0 da d4 00 00 00 04 00`).
pub const STATUS_READ_LEN: u16 = 4;
/// Size of the mode-acknowledgement read (`c0 da <val> 00 00 02 00`).
pub const ACK_READ_LEN: u16 = 2;

/// Threshold the driver uses for "device ready": it polls until the decoded
/// state code is strictly below this.
pub const STATE_READY_MAX: u8 = 6;

/// Number of attempts the driver makes per status poll.
pub const STATUS_POLL_ATTEMPTS: u32 = 10;
/// Delay between status-poll attempts, in milliseconds.
pub const STATUS_POLL_DELAY_MS: u64 = 10;

/// Build the chip-configuration command for a given chip ID.
///
/// For the FPC1022 (`0x4ec`) this is `a8 93|00 0f <arg>`; other chips get the
/// shorter `a8 0f <arg>` form. This is a *bulk* payload, not a control request.
pub fn chip_config(chip_id: u32, sub: u8) -> Vec<u8> {
    // The driver's flag byte is 0x93 when (something) is non-negative, else
    // 0x00. The exact predicate is not fully traced, so default to 0x93.
    const FLAG: u8 = 0x93;
    if chip_id == CHIP_ID_FPC1022 {
        vec![CMD_PXLCTRL, FLAG, 0x0f, sub]
    } else {
        vec![CMD_PXLCTRL, 0x0f, sub]
    }
}

/// Decode the 16-bit status word from a status-register reply.
///
/// The driver reads the two low bytes and combines them as `hi*0x100 + lo`
/// (i.e. little-endian 16-bit), then inspects individual bits.
pub fn decode_status_word(resp: &[u8]) -> Option<u16> {
    if resp.len() < 2 {
        return None;
    }
    Some(u16::from_le_bytes([resp[0], resp[1]]))
}

/// Individual bits of the status word, as the driver interprets them.
#[derive(Clone, Copy, Debug, Default)]
pub struct StatusBits {
    pub raw: u16,
}

impl StatusBits {
    pub fn from_word(w: u16) -> Self {
        StatusBits { raw: w }
    }
    pub fn bit(&self, i: u8) -> bool {
        (self.raw >> i) & 1 != 0
    }
}

impl std::fmt::Display for StatusBits {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "0x{:04x} [", self.raw)?;
        for i in (0..12).rev() {
            write!(f, "{}", if self.bit(i) { '1' } else { '0' })?;
        }
        write!(f, "]")
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
    let mut buf = vec![0u8; max.min(STAGE_LEN)];
    let n = handle
        .read_bulk(EP_IN, &mut buf, IMG_TIMEOUT)
        .context("bulk IN reply")?;
    buf.truncate(n);
    Ok(buf)
}

/// Issue a command on the **control** channel — the driver's real command path.
///
/// `bmRequestType = 0x40` (vendor, host-to-device), `bRequest = 0xdb`,
/// `wValue`/`wIndex` as given, `wLength = 0`. Recovered from
/// `fpc_wbdi.dll+0x180008eac`.
pub fn ctrl_write(
    handle: &DeviceHandle<rusb::Context>,
    w_value: u16,
    w_index: u16,
) -> Result<()> {
    handle
        .write_control(BM_WRITE, CREQ_WRITE, w_value, w_index, &[], CMD_TIMEOUT)
        .with_context(|| format!("control write wValue=0x{w_value:04x} wIndex=0x{w_index:04x}"))?;
    Ok(())
}

/// Read a reply from the **control** channel.
///
/// `bmRequestType = 0xc0` (vendor, device-to-host), `bRequest = 0xda`,
/// `wIndex = 0`. Recovered from `fpc_wbdi.dll+0x180008e3c`.
pub fn ctrl_read(
    handle: &DeviceHandle<rusb::Context>,
    w_value: u16,
    len: u16,
) -> Result<Vec<u8>> {
    let mut buf = vec![0u8; len as usize];
    let n = handle
        .read_control(BM_READ, CREQ_READ, w_value, 0, &mut buf, CMD_TIMEOUT)
        .with_context(|| format!("control read wValue=0x{w_value:04x} len={len}"))?;
    buf.truncate(n);
    Ok(buf)
}

/// Send a bulk payload the way the driver does: constant register frame, then
/// the `40 ca` bulk write carrying the payload. Used for image/streaming data.
pub fn write_bulk_command(handle: &DeviceHandle<rusb::Context>, payload: &[u8]) -> Result<()> {
    write_packet(handle, &REG_FRAME)?;
    write_packet(handle, &bulk_packet(payload))?;
    Ok(())
}

/// Perform the driver's chip bring-up on the control channel.
///
/// Writes [`INIT_474`] (and optionally [`INIT_474_IDX1`]), then reads the
/// acknowledgement. Returns the raw acknowledgement bytes.
pub fn chip_init(handle: &DeviceHandle<rusb::Context>, cmd: InitCmd) -> Result<Vec<u8>> {
    ctrl_write(handle, cmd.w_value, cmd.w_index)?;
    ctrl_read(handle, cmd.w_value, ACK_READ_LEN)
}

/// Read the status register (`c0 da d4 00 00 00 04 00`).
pub fn read_status(handle: &DeviceHandle<rusb::Context>) -> Result<Vec<u8>> {
    ctrl_read(handle, STATUS_REG_WVALUE, STATUS_READ_LEN)
}

/// Decode the finger-present state from a status reply.
///
/// The driver decodes `fngrpresentstatus` as a 16-bit word; this reports the
/// raw bits and a best-effort interpretation.
pub fn decode_status(resp: &[u8]) -> String {
    match decode_status_word(resp) {
        Some(w) => {
            let bits = StatusBits::from_word(w);
            let finger = bits.bit(0);
            format!(
                "{} (finger={})",
                bits,
                if finger { "present" } else { "absent" }
            )
        }
        None => match resp.first() {
            Some(&0x00) => "ok / no finger".to_string(),
            Some(&b) => format!("state 0x{b:02x}"),
            None => "empty reply".to_string(),
        },
    }
}
