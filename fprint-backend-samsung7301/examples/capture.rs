//! Capture one burst of frames with the Rust driver and match them.
//!
//! Run it with a finger on the sensor:
//!
//! ```sh
//! cargo run --example capture --release
//! ```

use std::time::Duration;

use fprint_backend_samsung7301::matcher;
use samsung7301::Sensor;

fn main() -> anyhow::Result<()> {
    let mut sensor = Sensor::open()?;
    sensor.init_working()?;

    let info = sensor.chip_info()?;
    println!(
        "chip: family {:#04x} revision {:#04x}, {}x{}",
        info.family, info.revision, info.width, info.height
    );

    println!("waiting for a finger ...");
    if !sensor.wait_for_finger_zones(Duration::from_secs(20))? {
        println!("no finger");
        return Ok(());
    }

    std::thread::sleep(Duration::from_millis(150));

    let frames = sensor.capture_burst(16, Duration::from_millis(80))?;
    println!("captured {} frames", frames.len());

    let pixels: Vec<Vec<u8>> = frames.iter().map(|f| f.data.clone()).collect();
    for (i, frame) in pixels.iter().enumerate() {
        println!("  frame {i:2}: stddev {:6.2}", stddev(frame));
    }

    let outcome = matcher::match_frames(&pixels, &[])?;
    println!(
        "matcher: quality {}, selected {} frame(s) {:?}",
        outcome.quality_max,
        outcome.selected.len(),
        outcome.selected
    );

    Ok(())
}

/// The standard deviation of the pixel values.
fn stddev(data: &[u8]) -> f64 {
    let mean = data.iter().map(|b| *b as f64).sum::<f64>() / data.len() as f64;
    let var = data.iter().map(|b| (*b as f64 - mean).powi(2)).sum::<f64>() / data.len() as f64;
    var.sqrt()
}
