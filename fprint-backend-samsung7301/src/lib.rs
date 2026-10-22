//! A native Rust backend for the Samsung `04e8:7301` fingerprint sensor.
//!
//! The sensor is an FPC (Fingerprint Cards AB) part that Samsung rebadges
//! under its own USB vendor ID.  It speaks the FPC "wbdi" transport, which
//! is also used by the sibling `04e8:7305` and `04e8:730b` readers.
//!
//! The USB side of the protocol is the [`samsung7301`] crate, which was
//! written by reverse engineering the official Samsung Windows driver
//! (`fpc_wbdi.dll` v3.23.0.15) for this laptop.  See
//! `docs/REVERSE-ENGINEERING.md` for the full account.
//!
//! # Why the matching is not done here
//!
//! The frames of this sensor do not carry enough minutiae for the
//! NBIS/bozorth3 path, so the matching is done by the real SourceAFIS
//! matcher, which runs as a separate program ([`matcher`]).  The print that
//! is stored is the selected native frames, and the matcher re-derives the
//! template from them every time.
//!
//! # Example
//!
//! ```no_run
//! use fprint_backend_samsung7301::Samsung7301Backend;
//! use fprint_core::{Backend, Device};
//!
//! # async fn run() -> fprint_core::Result<()> {
//! let backend = Samsung7301Backend::new();
//! for device in backend.enumerate().await? {
//!     println!("{}", device.info().name);
//! }
//! # Ok(())
//! # }
//! ```

use std::time::Duration;

use fprint_core::{
    Backend, Device, DeviceFeature, DeviceId, DeviceInfo, DriverId, EnrollProgress, Error,
    FingerStatus, IdentifyOutcome, Print, Result, RetryReason, ScanType, Template, VerifyOutcome,
};
use futures::channel::{mpsc, oneshot};
use futures::StreamExt;
use samsung7301::Sensor;

pub mod matcher;
pub mod rawprint;

/// The id of the device.
pub const DEVICE_ID: &str = "samsung7301";

/// The id of the driver.  A print is bound to it.
pub const DRIVER_ID: &str = "samsung7301";

/// The number of finger presentations a full enrolment needs.
pub const ENROLL_STAGES: u32 = 5;

/// The number of frames one press of the finger is captured as.
///
/// The burst dominates the time of an attempt, and the matcher selects at
/// most four of the frames, so a long burst only adds waiting time.
pub const BURST_FRAMES: usize = 16;

/// The delay between two frames of the burst.
pub const BURST_DELAY: Duration = Duration::from_millis(80);

/// How long the finger has to rest on the sensor before it is exposed.
pub const CONTACT_SETTLE: Duration = Duration::from_millis(150);

/// How long a single press of the finger may take.
pub const FINGER_TIMEOUT: Duration = Duration::from_secs(30);

/// The backend: it finds the sensor and hands out devices for it.
pub struct Samsung7301Backend;

impl Samsung7301Backend {
    /// A backend that looks for the sensor on the USB bus.
    pub fn new() -> Self {
        Self
    }
}

impl Default for Samsung7301Backend {
    fn default() -> Self {
        Self::new()
    }
}

impl Backend for Samsung7301Backend {
    type Device = Samsung7301;

    async fn enumerate(&self) -> Result<Vec<Self::Device>> {
        Ok(vec![Samsung7301::new()])
    }

    async fn open(&self, id: &DeviceId) -> Result<Self::Device> {
        if id.as_str() != DEVICE_ID {
            return Err(Error::NotFound);
        }
        Ok(Samsung7301::new())
    }
}

/// The reader.
pub struct Samsung7301 {
    info: DeviceInfo,
    sensor: Option<Sensor>,
}

impl Samsung7301 {
    /// A device that is not open yet.
    pub fn new() -> Self {
        Self {
            info: DeviceInfo::new(
                DeviceId::new(DEVICE_ID),
                DriverId::new(DRIVER_ID),
                "Samsung 04e8:7301 Fingerprint Sensor",
                ScanType::Press,
                DeviceFeature::VERIFY,
                ENROLL_STAGES,
            ),
            sensor: None,
        }
    }
}

impl Default for Samsung7301 {
    fn default() -> Self {
        Self::new()
    }
}

impl Device for Samsung7301 {
    fn info(&self) -> &DeviceInfo {
        &self.info
    }

    async fn open(&mut self) -> Result<()> {
        let mut sensor = Sensor::open().map_err(map_error)?;
        sensor.init_working().map_err(map_error)?;
        self.sensor = Some(sensor);
        Ok(())
    }

    async fn close(&mut self) -> Result<()> {
        if let Some(mut sensor) = self.sensor.take() {
            sensor.close();
        }
        Ok(())
    }

    async fn enroll<F: FnMut(EnrollProgress)>(
        &mut self,
        print: Print,
        mut on_progress: F,
    ) -> Result<Print> {
        let finger = print.finger;
        let (progress_tx, progress_rx) = mpsc::unbounded();
        let (result_tx, result_rx) = oneshot::channel();

        let mut sensor = self.sensor.take().ok_or(Error::ProtoState)?;
        let worker = std::thread::spawn(move || {
            let result = enrol_stages(&mut sensor, progress_tx);
            let _ = result_tx.send(result);
            sensor
        });

        // The progress is produced on the worker thread and reported here,
        // so the callback never leaves the thread that owns it.
        let forward = async move {
            let mut progress_rx = progress_rx;
            while let Some(progress) = progress_rx.next().await {
                on_progress(progress);
            }
        };
        let (result, ()) = futures::future::join(result_rx, forward).await;

        self.sensor = Some(worker.join().unwrap_or_else(|_| {
            panic!("the enrolment thread panicked")
        }));
        result.map_err(|_| Error::Other("the enrolment thread died".into()))?
            .map(|frames| build_print(frames, finger))
    }

    async fn verify_with_status<F: FnMut(FingerStatus)>(
        &mut self,
        enrolled: &Print,
        mut on_status: F,
    ) -> Result<VerifyOutcome> {
        let refs = match &enrolled.template {
            Template::Raw(data) => rawprint::parse(data).map_err(map_error)?,
            _ => return Err(Error::DataNotFound),
        };

        let (status_tx, status_rx) = mpsc::unbounded();
        let (result_tx, result_rx) = oneshot::channel();

        let mut sensor = self.sensor.take().ok_or(Error::ProtoState)?;
        let worker = std::thread::spawn(move || {
            let result = verify_press(&mut sensor, &refs, status_tx);
            let _ = result_tx.send(result);
            sensor
        });

        let forward = async move {
            let mut status_rx = status_rx;
            while let Some(status) = status_rx.next().await {
                on_status(status);
            }
        };
        let (result, ()) = futures::future::join(result_rx, forward).await;

        self.sensor = Some(worker.join().unwrap_or_else(|_| {
            panic!("the verification thread panicked")
        }));
        let matched = result.map_err(|_| Error::Other("the verification thread died".into()))?;
        Ok(VerifyOutcome::new(matched?, None))
    }

    async fn identify_with_status<F: FnMut(FingerStatus)>(
        &mut self,
        gallery: &[Print],
        on_status: F,
    ) -> Result<IdentifyOutcome> {
        // Only one print is enrolled per finger, so a gallery never holds
        // more than one usable entry for this device.
        let Some(enrolled) = gallery.first() else {
            return Err(Error::DataNotFound);
        };
        let outcome = self.verify_with_status(enrolled, on_status).await?;
        Ok(IdentifyOutcome::new(
            outcome.matched.then_some(0),
            outcome.scanned,
        ))
    }

    async fn suspend(&mut self) -> Result<()> {
        Ok(())
    }

    async fn resume(&mut self) -> Result<()> {
        Ok(())
    }
}

/// Enrol the finger, one stage at a time.
fn enrol_stages(
    sensor: &mut Sensor,
    progress: mpsc::UnboundedSender<EnrollProgress>,
) -> Result<Vec<Vec<u8>>> {
    let mut refs: Vec<Vec<u8>> = Vec::new();

    for stage in 0..ENROLL_STAGES {
        loop {
            if !sensor.wait_for_finger_zones(FINGER_TIMEOUT).map_err(map_error)? {
                let _ = progress.unbounded_send(
                    EnrollProgress::new(stage, ENROLL_STAGES).with_retry(RetryReason::General),
                );
                continue;
            }
            std::thread::sleep(CONTACT_SETTLE);

            let frames: Vec<Vec<u8>> = sensor
                .capture_burst(BURST_FRAMES, BURST_DELAY)
                .map_err(map_error)?
                .into_iter()
                .map(|image| image.data)
                .collect();

            let outcome = matcher::match_frames(&frames, &refs).map_err(map_error)?;

            // A stage is accepted when the capture was strong enough and,
            // from the second stage on, the new frames match the ones that
            // were enrolled before.
            let accepted = !outcome.selected.is_empty()
                && outcome.capture_is_strong()
                && (refs.is_empty() || outcome.matched());

            if accepted {
                for index in &outcome.selected {
                    refs.push(frames[*index].clone());
                }
                break;
            }

            let _ = progress.unbounded_send(
                EnrollProgress::new(stage, ENROLL_STAGES).with_retry(RetryReason::General),
            );
        }

        let _ = progress.unbounded_send(EnrollProgress::new(stage + 1, ENROLL_STAGES));
    }

    Ok(refs)
}

/// Verify one press of the finger against the enrolled frames.
fn verify_press(
    sensor: &mut Sensor,
    refs: &[Vec<u8>],
    status: mpsc::UnboundedSender<FingerStatus>,
) -> Result<bool> {
    let _ = status.unbounded_send(FingerStatus::NEEDED);

    if !sensor.wait_for_finger_zones(FINGER_TIMEOUT).map_err(map_error)? {
        return Ok(false);
    }
    let _ = status.unbounded_send(FingerStatus::PRESENT);
    std::thread::sleep(CONTACT_SETTLE);

    let frames: Vec<Vec<u8>> = sensor
        .capture_burst(BURST_FRAMES, BURST_DELAY)
        .map_err(map_error)?
        .into_iter()
        .map(|image| image.data)
        .collect();

    let _ = status.unbounded_send(FingerStatus::NONE);

    let outcome = matcher::match_frames(&frames, refs).map_err(map_error)?;
    Ok(outcome.matched())
}

/// Build the print that is stored.
fn build_print(frames: Vec<Vec<u8>>, finger: Option<fprint_core::Finger>) -> Print {
    let payload = rawprint::build(&frames).unwrap_or_default();
    let mut print = Print::default();
    print.template = Template::Raw(payload);
    print.finger = finger;
    print.driver = Some(DriverId::new(DRIVER_ID));
    print.device_id = Some(DeviceId::new(DEVICE_ID));
    print
}

/// Map an error of the USB side onto the error of the core.
fn map_error(error: anyhow::Error) -> Error {
    Error::Transport(format!("{error:#}"))
}
