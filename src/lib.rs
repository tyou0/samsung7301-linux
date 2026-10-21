//! Linux userspace driver for the Samsung `04e8:7301` fingerprint sensor.
//!
//! The sensor is an FPC (Fingerprint Cards AB) part that Samsung rebadges
//! under its own USB vendor ID. It speaks the FPC "wbdi" transport, which is
//! also used by the sibling `04e8:7305` and `04e8:730b` readers.
//!
//! Everything in this crate was recovered by reverse engineering the official
//! Samsung Windows driver (`fpc_wbdi.dll` v3.23.0.15) for this laptop and by
//! cross checking the result against a USB capture of the same driver family
//! published in libfprint merge request !586. See `docs/REVERSE-ENGINEERING.md`
//! for the full account.
//!
//! # Example
//!
//! ```no_run
//! use samsung7301::Sensor;
//!
//! let mut sensor = Sensor::open()?;
//! println!("chip: {:?}", sensor.chip_info()?);
//! sensor.init()?;
//! if sensor.wait_for_finger(std::time::Duration::from_secs(10))? {
//!     let image = sensor.capture()?;
//!     println!("{}x{} image", image.width, image.height);
//! }
//! # Ok::<(), anyhow::Error>(())
//! ```
//!
//! # Safety of the protocol
//!
//! Every reply is length checked before it is parsed; the driver never trusts
//! a length that the device supplied.

#![warn(missing_docs)]

pub mod protocol;
pub mod transport;

use std::time::{Duration, Instant};

use anyhow::{Context as _, Result};
use rusb::{Context, DeviceHandle, UsbContext};

use protocol::{Cmd, IMAGE_LEN, IMG_HEIGHT, IMG_WIDTH};
use transport::Transport;

/// A raw fingerprint image, one byte per pixel.
#[derive(Clone, Debug)]
pub struct Image {
    /// Image width in pixels.
    pub width: usize,
    /// Image height in pixels.
    pub height: usize,
    /// Raw pixel data, row major, `width * height` bytes.
    pub data: Vec<u8>,
}

impl Image {
    /// Mean pixel value.
    pub fn mean(&self) -> f64 {
        if self.data.is_empty() {
            return 0.0;
        }
        self.data.iter().map(|b| *b as f64).sum::<f64>() / self.data.len() as f64
    }

    /// Standard deviation of the pixel values.
    pub fn stddev(&self) -> f64 {
        if self.data.is_empty() {
            return 0.0;
        }
        let mean = self.mean();
        let var = self
            .data
            .iter()
            .map(|b| (*b as f64 - mean).powi(2))
            .sum::<f64>()
            / self.data.len() as f64;
        var.sqrt()
    }
}

/// What the sensor reports about itself.
#[derive(Clone, Debug)]
pub struct ChipInfo {
    /// Raw reply of the `hwid` command.
    pub hwid: Vec<u8>,
    /// FPC chip family byte (`0x02` is the FPC1020/1021 family).
    pub family: u8,
    /// Chip revision byte.
    pub revision: u8,
    /// Image width reported by the sensor.
    pub width: usize,
    /// Image height reported by the sensor.
    pub height: usize,
}

/// An open handle to the sensor.
pub struct Sensor {
    handle: DeviceHandle<Context>,
}

impl Sensor {
    /// Open the sensor and claim its interface.
    pub fn open() -> Result<Self> {
        let ctx = Context::new().context("initialising libusb")?;
        let device = {
            let devices = ctx.devices().context("enumerating USB devices")?;
            let mut found = None;
            for dev in devices.iter() {
                let desc = dev.device_descriptor().context("reading device descriptor")?;
                if desc.vendor_id() == protocol::VID && desc.product_id() == protocol::PID_7301 {
                    found = Some(dev);
                    break;
                }
            }
            found.context("no 04e8:7301 device found — is the sensor enabled in the BIOS?")?
        };

        let handle = device
            .open()
            .context("opening the device (permission denied? install contrib/99-samsung-7301.rules)")?;
        let _ = handle.set_auto_detach_kernel_driver(true);
        handle
            .claim_interface(0)
            .context("claiming interface 0 (is fprintd holding the device?)")?;
        Ok(Self { handle })
    }

    /// Open the sensor session (vendor control request `0xdb`).
    pub fn open_session(&mut self) -> Result<()> {
        Transport::new(&mut self.handle).session_open()
    }

    /// Read the chip identification and the capture geometry.
    pub fn chip_info(&mut self) -> Result<ChipInfo> {
        let mut t = Transport::new(&mut self.handle);
        t.session_open()?;
        let hwid = t.exchange(protocol::SEQ_HWID.bytes, protocol::SEQ_HWID.reply_len, false)?;
        // The reply is [ack, family, revision].
        let family = hwid.get(1).copied().unwrap_or(0);
        let revision = hwid.get(2).copied().unwrap_or(0);

        // The `imgcapsize` reply echoes the geometry that was in effect
        // before the command, so any value works to read it back.
        let geo = t.exchange(
            protocol::SEQ_CAPGEO.bytes,
            protocol::SEQ_CAPGEO.reply_len,
            false,
        )?;
        let height = geo.get(2).copied().unwrap_or(0) as usize;
        let width = geo.get(4).copied().unwrap_or(0) as usize;

        Ok(ChipInfo {
            hwid,
            family,
            revision,
            width,
            height,
        })
    }

    /// Run the bring-up sequence.
    ///
    /// This is the FPC1021 configuration table recovered from the Windows
    /// driver, plus the capture parameters that were tuned on real hardware.
    pub fn init(&mut self) -> Result<()> {
        let mut t = Transport::new(&mut self.handle);
        t.session_open()?;
        for cmd in protocol::INIT_SEQUENCE_1021 {
            t.exchange(cmd.bytes, cmd.reply_len, cmd.doorbell)
                .with_context(|| format!("init command {} failed", protocol::op_name(cmd.bytes[0])))?;
        }
        for cmd in protocol::TUNING_SEQUENCE {
            t.exchange(cmd.bytes, cmd.reply_len, cmd.doorbell)
                .with_context(|| format!("tuning command {} failed", protocol::op_name(cmd.bytes[0])))?;
        }
        Ok(())
    }

    /// Arm the sensor and wait until a finger is detected.
    ///
    /// Returns `true` when the sensor reports a finger, `false` on timeout.
    pub fn wait_for_finger(&mut self, timeout: Duration) -> Result<bool> {
        let mut t = Transport::new(&mut self.handle);
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            let _ = t.exchange(
                protocol::SEQ_ARM.bytes,
                protocol::SEQ_ARM.reply_len,
                true,
            )?;
            let status = t.exchange(
                protocol::SEQ_STATUS.bytes,
                protocol::SEQ_STATUS.reply_len,
                false,
            )?;
            if status.len() >= 2 && status[1] == protocol::STATUS_FINGER {
                return Ok(true);
            }
            std::thread::sleep(protocol::POLL_DELAY);
        }
        Ok(false)
    }

    /// Capture a single image.
    ///
    /// The sensor starts streaming the image while the host is still writing
    /// the command, so the read has to be armed before the write; see
    /// [`Transport::exchange_concurrent`].
    pub fn capture(&mut self) -> Result<Image> {
        let mut t = Transport::new(&mut self.handle);

        for cmd in protocol::CAPTURE_SETUP_FRAME {
            t.exchange(cmd.bytes, cmd.reply_len, cmd.doorbell)?;
        }

        // Trigger the exposure.
        t.exchange(
            protocol::SEQ_TRIGGER.bytes,
            protocol::SEQ_TRIGGER.reply_len,
            true,
        )?;

        // The image command is padded to the length of the transfer.
        let mut cmd = vec![0u8; IMAGE_LEN];
        cmd[0] = protocol::CMD_IMAGE_READ;
        let data = t.exchange_concurrent(&cmd, IMAGE_LEN)?;

        // The reply starts with a two byte header.
        let pixels = if data.len() > 2 {
            data[2..].to_vec()
        } else {
            Vec::new()
        };
        Ok(Image {
            width: IMG_WIDTH,
            height: IMG_HEIGHT,
            data: pixels,
        })
    }

    /// Release the interface.
    pub fn close(&mut self) {
        let _ = self.handle.release_interface(0);
    }

    /// Borrow the raw USB handle.
    pub fn handle_mut(&mut self) -> &mut DeviceHandle<Context> {
        &mut self.handle
    }
}

impl Drop for Sensor {
    fn drop(&mut self) {
        let _ = self.handle.release_interface(0);
    }
}

/// Helper for building a [`Cmd`] from raw bytes.
pub fn cmd(bytes: &'static [u8]) -> Cmd {
    Cmd::new(bytes, bytes.len() as u16)
}
