//! USB transport for the Samsung `04e8:7301` fingerprint sensor.
//!
//! # Wire format
//!
//! The sensor speaks the FPC "wbdi" transport used by the whole Samsung FPC
//! reader family (`04e8:7301`, `04e8:7305`, `04e8:730b`). Every command is
//! bracketed by a fixed sequence of vendor control transfers on EP0:
//!
//! ```text
//! 1. CTRL-OUT  bRequest=0xc3 wValue=0x0000 wIndex=0x0000  data=16-byte heartbeat
//! 2. CTRL-OUT  bRequest=0xca wValue=0x0003 wIndex=<reply_len>
//! 3. BULK-OUT  <command bytes>            (EP 0x01)
//! 4. BULK-IN   <reply_len> bytes          (EP 0x82)
//! 5. CTRL-IN   bRequest=0xda wValue=0x0007 wIndex=0x0000  2 bytes  (doorbell)
//! ```
//!
//! Steps 1, 2 and 5 are vendor control requests (`bmRequestType` 0x40 for
//! host-to-device, 0xc0 for device-to-host). Step 2 tells the device how many
//! bytes to stage on the bulk IN pipe; step 5 is required for a handful of
//! opcodes (`0x20` arm, `0x28` init-complete, `0xc0` trigger, `0xf8` reset).
//!
//! The session is opened with vendor control writes `bRequest=0xdb` carrying
//! `wValue` 0x0006 and 0x000b and `wIndex` 0 and 1.
//!
//! Provenance: the same transport was independently recovered from USB
//! captures of the Samsung Windows driver in libfprint MR !586
//! (`drivers/samsung7305.c`); see `docs/REVERSE-ENGINEERING.md`.

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use anyhow::{bail, Context as _, Result};
use rusb::{Context, DeviceHandle, UsbContext};

use libusb1_sys as sys;

/// Set by the bulk-IN transfer callback.
static IN_DONE: AtomicBool = AtomicBool::new(false);
/// Set by the bulk-OUT transfer callback.
static OUT_DONE: AtomicBool = AtomicBool::new(false);

extern "system" fn in_callback(_transfer: *mut sys::libusb_transfer) {
    IN_DONE.store(true, Ordering::SeqCst);
}

extern "system" fn out_callback(_transfer: *mut sys::libusb_transfer) {
    OUT_DONE.store(true, Ordering::SeqCst);
}

/// Bulk OUT endpoint (host -> device).
pub const EP_OUT: u8 = 0x01;
/// Bulk IN endpoint (device -> host).
pub const EP_IN: u8 = 0x82;
/// Interrupt IN endpoint (status / doorbell).
pub const EP_INTR: u8 = 0x83;

/// Timeout for control transfers.
pub const CTRL_TIMEOUT: Duration = Duration::from_millis(2_000);
/// Timeout for a command's bulk reply.
pub const CMD_TIMEOUT: Duration = Duration::from_millis(2_000);
/// Timeout for image transfers.
pub const IMG_TIMEOUT: Duration = Duration::from_millis(5_000);

/// `bmRequestType` for a vendor, host-to-device request.
pub const BM_WRITE: u8 = 0x40;
/// `bmRequestType` for a vendor, device-to-host request.
pub const BM_READ: u8 = 0xc0;

/// Vendor request: "start a bulk exchange" (`wValue` = frame type 0x0003).
pub const CREQ_BULK: u8 = 0xca;
/// Vendor request: session/frame heartbeat, host to device.
pub const CREQ_HEARTBEAT: u8 = 0xc3;
/// Vendor request: doorbell / status, device to host.
pub const CREQ_STATUS: u8 = 0xda;
/// Vendor request: session control, host to device.
pub const CREQ_SESSION: u8 = 0xdb;

/// Frame type used in the `0xca` bulk request (`wValue`).
pub const FRAME_TYPE: u16 = 0x0003;

/// `wValue` of the doorbell read.
pub const DOORBELL_WVALUE: u16 = 0x0007;

/// The 16-byte heartbeat that precedes every bulk exchange.
///
/// Captured verbatim from the Samsung Windows driver traffic; identical on
/// every `04e8` FPC reader of this family.
pub const HEARTBEAT: [u8; 16] = [
    0xc0, 0xc6, 0x2d, 0x00, 0x08, 0x00, 0xff, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
];

/// A low-level handle on the sensor.
pub struct Transport<'a> {
    handle: &'a mut DeviceHandle<Context>,
}

impl<'a> Transport<'a> {
    pub fn new(handle: &'a mut DeviceHandle<Context>) -> Self {
        Self { handle }
    }

    /// Vendor control transfer, host to device.
    pub fn ctrl_out(&mut self, request: u8, value: u16, index: u16, data: &[u8]) -> Result<()> {
        self.handle
            .write_control(
                BM_WRITE,
                request,
                value,
                index,
                data,
                Duration::from_millis(2_000),
            )
            .with_context(|| {
                format!("control OUT bRequest=0x{request:02x} wValue=0x{value:04x} wIndex=0x{index:04x}")
            })?;
        Ok(())
    }

    /// Vendor control transfer, device to host.
    pub fn ctrl_in(&mut self, request: u8, value: u16, index: u16, len: u16) -> Result<Vec<u8>> {
        let mut buf = vec![0u8; len as usize];
        let n = self
            .handle
            .read_control(
                BM_READ,
                request,
                value,
                index,
                &mut buf,
                Duration::from_millis(2_000),
            )
            .with_context(|| {
                format!("control IN bRequest=0x{request:02x} wValue=0x{value:04x} len={len}")
            })?;
        buf.truncate(n);
        Ok(buf)
    }

    /// Bulk write to the command pipe.
    pub fn bulk_out(&mut self, data: &[u8], timeout: Duration) -> Result<usize> {
        self.handle
            .write_bulk(EP_OUT, data, timeout)
            .with_context(|| format!("bulk OUT {} bytes", data.len()))
    }

    /// Bulk read from the data pipe.
    pub fn bulk_in(&mut self, len: usize, timeout: Duration) -> Result<Vec<u8>> {
        let mut buf = vec![0u8; len];
        let n = self
            .handle
            .read_bulk(EP_IN, &mut buf, timeout)
            .context("bulk IN")?;
        buf.truncate(n);
        Ok(buf)
    }

    /// Open the sensor session.
    ///
    /// The Windows driver issues four `0xdb` writes before its first
    /// command: `wValue` 0x0006 and 0x000b, each with `wIndex` 0 and 1.
    /// Only the 0x0006 pair is mandatory; the 0x000b pair is a follow-up
    /// re-init that some parts require.
    pub fn session_open(&mut self) -> Result<()> {
        for value in [0x0006u16, 0x000b] {
            for index in [0u16, 1] {
                // A failure here is not fatal: the 0x000b pair is a re-init
                // that not every chip variant implements.
                match self.ctrl_out(CREQ_SESSION, value, index, &[]) {
                    Ok(()) => {}
                    Err(e) => {
                        if value == 0x0006 {
                            return Err(e);
                        }
                    }
                }
            }
        }
        Ok(())
    }

    /// Run one complete command exchange.
    ///
    /// `prime_len` is the number of bytes the device is expected to stage on
    /// the bulk IN pipe. `doorbell` requests the extra status read that a few
    /// opcodes (`0x20`, `0x28`, `0xc0`, `0xf8`) require.
    pub fn exchange(&mut self, cmd: &[u8], prime_len: u16, doorbell: bool) -> Result<Vec<u8>> {
        // 1. heartbeat
        self.ctrl_out(CREQ_HEARTBEAT, 0x0000, 0x0000, &HEARTBEAT)?;

        // 2. prime the reply length
        self.ctrl_out(CREQ_BULK, FRAME_TYPE, prime_len, &[])?;

        // 3. the command itself
        self.bulk_out(cmd, CMD_TIMEOUT)?;

        // 4. the reply
        let reply = self.bulk_in(prime_len as usize, CMD_TIMEOUT)?;

        // 5. optional doorbell
        if doorbell {
            let _ = self.ctrl_in(CREQ_STATUS, DOORBELL_WVALUE, 0x0000, 2)?;
        }

        Ok(reply)
    }

    /// Read the current sensor status word (`0xda` with `wValue=0xd4`).
    pub fn status_word(&mut self) -> Result<Vec<u8>> {
        self.ctrl_in(CREQ_STATUS, 0x00d4, 0x0000, 4)
    }

    /// Drain whatever the device has staged on the bulk IN pipe.
    pub fn drain(&mut self) -> Result<Vec<u8>> {
        let mut buf = vec![0u8; 16 * 1024];
        match self.handle.read_bulk(EP_IN, &mut buf, Duration::from_millis(50)) {
            Ok(n) => {
                buf.truncate(n);
                Ok(buf)
            }
            Err(rusb::Error::Timeout) => Ok(Vec::new()),
            Err(e) => Err(e).context("bulk IN drain"),
        }
    }

    /// Read the interrupt endpoint (unsolicited status / doorbell).
    pub fn read_interrupt(&mut self, timeout: Duration) -> std::result::Result<Vec<u8>, rusb::Error> {
        let mut buf = vec![0u8; 64];
        let n = self.handle.read_interrupt(EP_INTR, &mut buf, timeout)?;
        buf.truncate(n);
        Ok(buf)
    }

    /// Send a command and read the reply **concurrently**.
    ///
    /// The sensor starts staging the reply on the bulk IN pipe while the
    /// host is still writing the command to the bulk OUT pipe, so the read
    /// has to be armed before the write. Doing the write first loses the
    /// beginning of the transfer.
    pub fn exchange_concurrent(&mut self, cmd: &[u8], reply_len: usize) -> Result<Vec<u8>> {
        self.ctrl_out(CREQ_HEARTBEAT, 0x0000, 0x0000, &HEARTBEAT)?;
        self.ctrl_out(CREQ_BULK, FRAME_TYPE, reply_len as u16, &[])?;

        let raw_handle = self.handle.as_raw();
        let raw_ctx = self.handle.context().as_raw();

        // The command buffer has to outlive the transfer.
        let mut cmd_buf = cmd.to_vec();
        let mut in_buf = vec![0u8; reply_len];

        unsafe {
            let in_tr = sys::libusb_alloc_transfer(0);
            let out_tr = sys::libusb_alloc_transfer(0);
            if in_tr.is_null() || out_tr.is_null() {
                bail!("libusb_alloc_transfer failed");
            }

            IN_DONE.store(false, Ordering::SeqCst);
            OUT_DONE.store(false, Ordering::SeqCst);

            sys::libusb_fill_bulk_transfer(
                in_tr,
                raw_handle,
                EP_IN,
                in_buf.as_mut_ptr(),
                reply_len as i32,
                in_callback,
                std::ptr::null_mut(),
                IMG_TIMEOUT.as_millis() as u32,
            );
            sys::libusb_fill_bulk_transfer(
                out_tr,
                raw_handle,
                EP_OUT,
                cmd_buf.as_mut_ptr(),
                cmd_buf.len() as i32,
                out_callback,
                std::ptr::null_mut(),
                IMG_TIMEOUT.as_millis() as u32,
            );

            let rc = sys::libusb_submit_transfer(in_tr);
            if rc != 0 {
                sys::libusb_free_transfer(in_tr);
                sys::libusb_free_transfer(out_tr);
                bail!("libusb_submit_transfer(IN) failed: {rc}");
            }
            let rc = sys::libusb_submit_transfer(out_tr);
            if rc != 0 {
                sys::libusb_cancel_transfer(in_tr);
                sys::libusb_free_transfer(in_tr);
                sys::libusb_free_transfer(out_tr);
                bail!("libusb_submit_transfer(OUT) failed: {rc}");
            }

            // Wait for both transfers to complete.
            let tv = libc::timeval {
                tv_sec: 0,
                tv_usec: 100_000,
            };
            let deadline = Instant::now() + IMG_TIMEOUT;
            while !(IN_DONE.load(Ordering::SeqCst) && OUT_DONE.load(Ordering::SeqCst)) {
                if Instant::now() > deadline {
                    sys::libusb_cancel_transfer(in_tr);
                    sys::libusb_cancel_transfer(out_tr);
                    sys::libusb_free_transfer(in_tr);
                    sys::libusb_free_transfer(out_tr);
                    bail!("timed out waiting for the image transfer");
                }
                sys::libusb_handle_events_timeout(raw_ctx, &tv);
            }

            let out_len = (*out_tr).actual_length;
            let in_len = (*in_tr).actual_length;
            let in_status = (*in_tr).status;
            sys::libusb_free_transfer(in_tr);
            sys::libusb_free_transfer(out_tr);

            if in_status != 0 {
                bail!("bulk IN failed with status {in_status}");
            }
            if in_len < 0 || out_len < 0 {
                bail!("negative transfer length");
            }
            in_buf.truncate(in_len as usize);
            Ok(in_buf)
        }
    }
}

/// Format a byte slice as space separated hex.
pub fn hex(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<Vec<_>>()
        .join(" ")
}

/// Bail out with a helpful message if the device does not answer.
pub fn require_nonempty(resp: &[u8], what: &str) -> Result<()> {
    if resp.is_empty() {
        bail!("{what}: empty reply");
    }
    Ok(())
}
