//! Wire protocol for the Samsung `04e8:7301` fingerprint sensor.
//!
//! # Provenance
//!
//! The sensor is an FPC (Fingerprint Cards AB) part that Samsung rebadges
//! under vendor ID `04e8`. The official Windows driver for this exact laptop
//! (`fpc_wbdi.dll`, v3.23.0.15) was disassembled to recover the command
//! vocabulary below; see `docs/REVERSE-ENGINEERING.md`.
//!
//! The transport (see [`crate::transport`]) is the "wbdi" framing shared by
//! the whole Samsung FPC reader family.

// The opcode table below is self-documenting: every constant is listed with
// its name in the driver's dispatcher, and the table is reproduced in
// `docs/REVERSE-ENGINEERING.md`.
#![allow(missing_docs)]

use std::time::Duration;

/// Samsung USB vendor ID.
pub const VID: u16 = 0x04e8;
/// The sensor this crate targets.
pub const PID_7301: u16 = 0x7301;
/// Sibling sensor sharing the same driver (for reference/experiments).
pub const PID_7305: u16 = 0x7305;
/// Sibling area sensor with the same transport (reference driver).
pub const PID_730B: u16 = 0x730b;

/// Chip ID of the FPC1022, the part this laptop carries.
pub const CHIP_ID_FPC1022: u32 = 0x4ec;

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

/// Opcodes that require the extra `0xda` doorbell read after the exchange.
pub fn needs_doorbell(op: u8) -> bool {
    matches!(
        op,
        CMD_FINGER_PRESENT_QUERY
            | CMD_ACTIVATE_SLEEP_MODE
            | CMD_SOFT_RESET
            | CMD_CAPTURE_IMAGE
    )
}

// ---------------------------------------------------------------------------
// Register configuration
// ---------------------------------------------------------------------------

/// Register configuration words for chip ID `0x4ec` (FPC1022), from
/// `fpc_wbdi.dll+0x1800071d8`. Little-endian 16-bit words; `0x0100`
/// terminates a frame.
///
/// The pairs are `register`, `value`; the driver's walker turns each pair
/// into a register write.
pub const REG_CONFIG_4EC: &[u16] = &[
    0x004c, 0x0004, 0x00ff, 0x00ff, 0x0011, 0x0100,
    0x006c, 0x0004, 0x0100, 0x0068, 0x0013, 0x0008, 0x0008, 0x0008, 0x0008, 0x0014, 0x0014, 0x0014,
    0x0014, 0x0100,
    0x0080, 0x0082, 0x0015, 0x0015, 0x0015, 0x0015, 0x0009, 0x0009, 0x0009, 0x0009, 0x0100,
    0x0098, 0x0003, 0x00eb, 0x003f, 0x00b8, 0x000a, 0x0007, 0x0011, 0x0022, 0x006a, 0x005d, 0x0100,
    0x008c, 0x0000, 0x0004,
];

/// Split [`REG_CONFIG_4EC`] into the driver's `40 c3` register frames, flushing
/// at each `0x0100` terminator.
pub fn reg_frames(words: &[u16]) -> Vec<Vec<u8>> {
    let mut frames = Vec::new();
    let mut current = Vec::new();
    for &w in words {
        if w == 0x0100 {
            if !current.is_empty() {
                frames.push(std::mem::take(&mut current));
            }
        } else {
            current.extend_from_slice(&w.to_le_bytes());
        }
    }
    if !current.is_empty() {
        frames.push(current);
    }
    frames
}

// ---------------------------------------------------------------------------
// Init / capture sequences
// ---------------------------------------------------------------------------

/// A command with the reply length the device is expected to stage.
#[derive(Clone, Copy, Debug)]
pub struct Cmd {
    pub bytes: &'static [u8],
    pub reply_len: u16,
    pub doorbell: bool,
}

impl Cmd {
    pub const fn new(bytes: &'static [u8], reply_len: u16) -> Self {
        Self {
            bytes,
            reply_len,
            doorbell: false,
        }
    }

    pub const fn with_doorbell(bytes: &'static [u8], reply_len: u16) -> Self {
        Self {
            bytes,
            reply_len,
            doorbell: true,
        }
    }
}

/// Soft reset.
pub const SEQ_RESET: Cmd = Cmd::with_doorbell(&[0xf8], 1);
/// Read interrupt status with clear (finger present bit lives here).
pub const SEQ_STATUS: Cmd = Cmd::new(&[0x1c, 0x00], 2);
/// Hardware ID / chip identification.
pub const SEQ_HWID: Cmd = Cmd::new(&[0xfc, 0x00, 0x00], 3);
/// Image capture geometry.
pub const SEQ_CAPGEO: Cmd = Cmd::new(&[0x54, 0x50, 0x18, 0x00, 0x38], 5);
/// Capacitive read (finger presence "saturation" check).
pub const SEQ_CAP: Cmd = Cmd::new(&[0xd4, 0x00, 0x00, 0x00, 0x00], 5);
/// Arm the sensor for a capture.
pub const SEQ_ARM: Cmd = Cmd::with_doorbell(&[0x20], 1);
/// Trigger a capture.
pub const SEQ_TRIGGER: Cmd = Cmd::with_doorbell(&[0xc0], 1);
/// Activate sleep mode (marks the end of the init sequence).
pub const SEQ_INITDONE: Cmd = Cmd::with_doorbell(&[0x28], 1);

/// The bring-up sequence, byte for byte as the Samsung Windows driver issues
/// it (recovered from a USB capture of a `04e8:7305`, whose Windows driver is
/// the same `fpc_wbdi` stack this machine uses).
///
/// The `wIndex` of the `0xca` prime — the expected reply length — equals the
/// length of the command itself.
pub const INIT_SEQUENCE: &[Cmd] = &[
    SEQ_RESET,
    SEQ_STATUS,
    SEQ_STATUS,
    SEQ_HWID,
    Cmd::new(&[0x90, 0x34], 2),
    Cmd::new(
        &[0x68, 0x33, 0x33, 0x33, 0x33, 0x3f, 0x3f, 0x3f, 0x3f],
        9,
    ),
    Cmd::new(&[0x9c, 0x55, 0x40, 0x00, 0x2d, 0x24], 6),
    Cmd::new(&[0x6c, 0x2b], 2),
    Cmd::new(&[0x8c, 0x12], 2),
    Cmd::new(&[0x5c, 0x0b], 2),
    Cmd::new(
        &[0x98, 0x03, 0xea, 0xbf, 0x68, 0x02, 0x09, 0x15, 0x2a, 0x7a, 0xde],
        11,
    ),
    Cmd::new(&[0xd8, 0x00, 0x01, 0x20, 0x20], 5),
    Cmd::new(&[0xe0, 0x32, 0x70, 0x22], 4),
    Cmd::new(&[0xe4, 0x32, 0x88, 0x70], 4),
    Cmd::new(&[0xe8, 0x32, 0x88], 3),
    Cmd::new(&[0xdc, 0x19, 0x01, 0x00, 0x0a], 5),
    Cmd::new(&[0xa8, 0x0f, 0x0a], 3),
    Cmd::new(&[0xa0, 0x0a, 0x02], 3),
    SEQ_INITDONE,
];

/// Bring-up sequence for the FPC1021 (`0x3fd`) part — the 160x160 sensor this
/// laptop carries.
///
/// The frames are the per-chip configuration table the Windows driver builds
/// in `fpc_wbdi.dll+0x180005fa8`; the walker at `+0x180007dd8` turns the
/// 16-bit word table into "register, value, value, ..." frames, terminated by
/// the word `0x0100`.
pub const INIT_SEQUENCE_1021: &[Cmd] = &[
    SEQ_RESET,
    SEQ_STATUS,
    SEQ_STATUS,
    SEQ_HWID,
    // adcsetup
    Cmd::new(&[0x9c, 0x55, 0x40, 0x00, 0x2d, 0x24], 6),
    // pxlrstdly
    Cmd::new(&[0x6c, 0x29, 0x00, 0x90, 0x20], 5),
    // sampledly
    Cmd::new(&[0x68, 0x31, 0x31, 0x3d, 0x3d], 5),
    // adcweighttbl (calibration)
    Cmd::new(
        &[0x98, 0x03, 0xeb, 0x3f, 0xb8, 0x0a, 0x70, 0x11, 0x22, 0x6a, 0x5d],
        11,
    ),
    // fngrdriveconf
    Cmd::new(&[0x8c, 0x12, 0x00, 0x5c, 0xb0], 5),
    // fngrdetthresh
    Cmd::new(&[0xd8, 0x20, 0x00], 3),
    // fngrdetcntr
    Cmd::new(&[0xdc, 0x00, 0x0a], 3),
    // imagesetup
    Cmd::new(&[0x5c, 0x0b], 2),
    // pxlctrl
    Cmd::new(&[0xa8, 0x0f, 0x0a], 3),
    // adcshiftgain
    Cmd::new(&[0xa0, 0x0a, 0x02], 3),
    SEQ_INITDONE,
];

/// Capture setup for a strip ("finger present" preview) read.
pub const CAPTURE_SETUP: &[Cmd] = &[
    SEQ_CAP,
    SEQ_CAPGEO,
    Cmd::new(&[0xa0, 0x10, 0x00], 3),
    Cmd::new(&[0xa8, 0x0f, 0x1a], 3),
    Cmd::new(&[0x64, 0x0c], 2),
    Cmd::new(&[0x5c, 0x0a], 2),
];

/// Capture setup for a full frame read: the capacitive read and the capture
/// geometry.  The analogue front end settings are applied separately (see
/// [`TUNING_SEQUENCE`]) and must not be overwritten here.
pub const CAPTURE_SETUP_FRAME: &[Cmd] = &[SEQ_CAP, SEQ_FRAME_GEO];

/// The capture setup to use (`strip` or `frame`).
pub fn capture_setup() -> &'static [Cmd] {
    match std::env::var("SAMSUNG7301_SETUP").as_deref() {
        Ok("frame") => CAPTURE_SETUP_FRAME,
        _ => CAPTURE_SETUP,
    }
}

/// Image read opcode. The command is padded to the length of the image
/// transfer, exactly as the Windows driver does.
pub const CMD_IMAGE_READ: u8 = 0xc4;

/// Capture geometry, as reported by the sensor's `imgcapsize` command.
///
/// The FPC1022 in this laptop reports a 160 x 160 frame, which matches the
/// `0x6402` byte staging buffer the Windows driver allocates (25600 pixels
/// plus a two byte header).
pub const IMG_WIDTH: usize = 160;
pub const IMG_HEIGHT: usize = 160;

/// Length of the image transfer, in bytes (image payload plus a two byte
/// header).
pub const IMAGE_LEN: usize = IMG_WIDTH * IMG_HEIGHT + 2;

/// Command that sets the capture geometry to the full frame.
///
/// The layout is `54 <mode> 0x18 <height> <width>`: the third byte is a
/// constant in the Windows driver, the fourth is the height and the fifth
/// the width.
pub const SEQ_FRAME_GEO: Cmd = Cmd::new(&[0x54, 0x00, 0xa0, 0x00, 0xa0], 5);

/// Capture parameters tuned on real hardware.
///
/// The Windows driver ships a configuration for the FPC1021 revision `0x10`;
/// this machine carries revision `0x1f`, whose ADC settings differ. These
/// values were found by sweeping the registers that control the analogue
/// front end and picking the setting with the largest image contrast:
///
/// * `a0 00 00` — `adcshiftgain`, gain 0. The driver's default gain of
///   `0x10` saturates the sensor at 246/255, so only a handful of pixels
///   carry any information at all.
/// * `9c 55 40 00 08 24` — `adcsetup`. The fourth argument is the one that
///   matters: the driver's value of `0x2d` leaves a flat field, `0x08` gives
///   a usable dynamic range.
/// * `64 12` — `imagerd`, the readout mode with the best contrast.
pub const TUNING_SEQUENCE: &[Cmd] = &[
    Cmd::new(&[0xa0, 0x00, 0x00], 3),
    Cmd::new(&[0x9c, 0x55, 0x40, 0x00, 0x08, 0x24], 6),
    Cmd::new(&[0x64, 0x12], 2),
];

/// Delay between finger-present polls.
pub const POLL_DELAY: Duration = Duration::from_millis(30);
/// Number of polls before giving up on a finger.
pub const POLL_ATTEMPTS: u32 = 2_000;

// ---------------------------------------------------------------------------
// Status decoding
// ---------------------------------------------------------------------------

/// Finger-present value of the `0x1c 0x00` status reply.
pub const STATUS_FINGER: u8 = 0x81;
/// Idle value of the `0x1c 0x00` status reply.
pub const STATUS_IDLE: u8 = 0x80;
/// Ready value of the `0x1c 0x00` status reply.
pub const STATUS_READY: u8 = 0x20;

/// Interpret the two bytes of a `0x1c 0x00` status reply.
pub fn decode_status(resp: &[u8]) -> String {
    match resp.len() {
        0 => "empty".into(),
        1 => format!("0x{:02x}", resp[0]),
        _ => {
            let state = match resp[1] {
                STATUS_FINGER => "finger present",
                STATUS_IDLE => "idle",
                STATUS_READY => "ready",
                other => {
                    return format!(
                        "0x{:02x} 0x{:02x} (state 0x{other:02x})",
                        resp[0], resp[1]
                    )
                }
            };
            format!("0x{:02x} 0x{:02x} ({state})", resp[0], resp[1])
        }
    }
}
