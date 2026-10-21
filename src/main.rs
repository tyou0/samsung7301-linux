//! `samsung7301` — command line tool for the Samsung `04e8:7301` fingerprint
//! sensor.
//!
//! The default mode is read-only. Command-sending modes are opt-in because
//! the protocol was recovered by reverse engineering the Samsung Windows
//! driver.

use std::time::Duration;

use anyhow::{bail, Context as _, Result};
use rusb::{Context, Device, DeviceHandle, UsbContext};

use samsung7301::protocol::{self, op_name};
use samsung7301::transport::{hex, Transport};
use samsung7301::Sensor;

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mode = args.first().map(String::as_str).unwrap_or("info");

    if matches!(mode, "-h" | "--help" | "help") {
        print_help();
        return Ok(());
    }

    if mode == "info" {
        let ctx = Context::new().context("initialising libusb")?;
        let dev = find_device(&ctx)?;
        println!("Found Samsung fingerprint device:");
        describe(&dev)?;
        println!("\nRead-only inspection complete. No commands were sent.");
        return Ok(());
    }

    let mut sensor = Sensor::open()?;

    match mode {
        "session" => {
            println!("\nopening session (0xdb 0x0006/0, 0x0006/1, 0x000b/0, 0x000b/1) ...");
            sensor.open_session()?;
            println!("  ok");
            let info = sensor.chip_info()?;
            println!("\nchip:");
            println!("  hwid reply  {}", hex(&info.hwid));
            println!("  family      0x{:02x}", info.family);
            println!("  revision    0x{:02x}", info.revision);
            println!("  geometry    {}x{}", info.width, info.height);
        }
        "init" => {
            println!("\nrunning the bring-up sequence ...");
            sensor.init()?;
            println!("  ok");
        }
        "capture" => {
            println!("\ninitialising ...");
            sensor.init()?;
            println!("  waiting for a finger ...");
            if !sensor.wait_for_finger(Duration::from_secs(30))? {
                bail!("no finger detected");
            }
            println!("  finger detected, capturing ...");
            let image = sensor.capture()?;
            println!(
                "  {}x{} image, mean {:.1}, stddev {:.2}",
                image.width,
                image.height,
                image.mean(),
                image.stddev()
            );
            std::fs::create_dir_all("captures").ok();
            std::fs::write("captures/raw.bin", &image.data)?;
            println!("  written to captures/raw.bin");
        }
        "cmd" => {
            let bytes = parse_hex(args.get(1).map(String::as_str).unwrap_or(""))?;
            let reply_len: u16 = args
                .get(2)
                .map(|s| parse_num(s) as u16)
                .unwrap_or(bytes.len() as u16);
            let doorbell = args.get(3).map(|s| s == "1").unwrap_or(false);
            let mut t = Transport::new(sensor_handle(&mut sensor));
            t.session_open()?;
            println!("sending {:<24} reply_len={reply_len}", hex(&bytes));
            let resp = t.exchange(&bytes, reply_len, doorbell)?;
            println!("  reply ({} bytes): {}", resp.len(), hex(&resp));
            if !bytes.is_empty() {
                println!("  opcode: {}", op_name(bytes[0]));
            }
        }
        "poll" => {
            let bytes = parse_hex(args.get(1).map(String::as_str).unwrap_or("d400000000"))?;
            let reply_len: u16 = args
                .get(2)
                .map(|s| parse_num(s) as u16)
                .unwrap_or(bytes.len() as u16);
            let mut t = Transport::new(sensor_handle(&mut sensor));
            t.session_open()?;
            for _ in 0..40 {
                match t.exchange(&bytes, reply_len, false) {
                    Ok(resp) => println!("  {}", hex(&resp)),
                    Err(e) => println!("  error {e:#}"),
                }
                std::thread::sleep(Duration::from_millis(200));
            }
        }
        other => {
            eprintln!("unknown mode: {other:?}\n");
            print_help();
            std::process::exit(2);
        }
    }

    sensor.close();
    Ok(())
}

fn print_help() {
    println!(
        "samsung7301 — driver for the Samsung 04e8:7301 fingerprint sensor

USAGE:
    samsung7301 [MODE] [ARGS]

MODES:
    info       (default) print descriptors/endpoints; sends nothing
    session    open the session and print the chip identification
    init       run the bring-up sequence
    capture    capture one image into captures/raw.bin
    cmd <hex> [reply_len] [doorbell]
              send one raw command, e.g. `cmd 1c00 2`
    poll <hex> [reply_len]
              send a command every 200 ms and print the replies

NOTES:
    The transport is the Samsung FPC \"wbdi\" framing: vendor control 0xc3
    (heartbeat) and 0xca (prime the reply length), then a bulk OUT of the
    command and a bulk IN of the reply. See docs/REVERSE-ENGINEERING.md."
    );
}

fn find_device(ctx: &Context) -> Result<Device<Context>> {
    for dev in ctx.devices().context("enumerating USB devices")?.iter() {
        let desc = dev.device_descriptor().context("reading device descriptor")?;
        if desc.vendor_id() == protocol::VID && desc.product_id() == protocol::PID_7301 {
            return Ok(dev);
        }
    }
    bail!(
        "no device {:04x}:{:04x} found — is the sensor enabled in the BIOS?",
        protocol::VID,
        protocol::PID_7301
    )
}

fn describe(dev: &Device<Context>) -> Result<()> {
    let d = dev.device_descriptor()?;
    println!(
        "  id        {vid:04x}:{pid:04x}  (usb {usb:?}, device {dev_ver:?})",
        vid = d.vendor_id(),
        pid = d.product_id(),
        usb = d.usb_version(),
        dev_ver = d.device_version(),
    );
    println!(
        "  class     {:02x}/{:02x}/{:02x}  max packet0 {}",
        d.class_code(),
        d.sub_class_code(),
        d.protocol_code(),
        d.max_packet_size()
    );

    let cfg = dev.config_descriptor(0).context("reading config descriptor")?;
    for intf in cfg.interfaces() {
        for desc in intf.descriptors() {
            println!(
                "  interface {} alt {} class {:02x}/{:02x}/{:02x}",
                desc.interface_number(),
                desc.setting_number(),
                desc.class_code(),
                desc.sub_class_code(),
                desc.protocol_code()
            );
            for ep in desc.endpoint_descriptors() {
                println!(
                    "    endpoint 0x{:02x}  {:?}  max packet {}  interval {}",
                    ep.address(),
                    ep.transfer_type(),
                    ep.max_packet_size(),
                    ep.interval()
                );
            }
        }
    }
    Ok(())
}

/// Borrow the raw handle of an open sensor.
fn sensor_handle(sensor: &mut Sensor) -> &mut DeviceHandle<Context> {
    sensor.handle_mut()
}

/// Parse a hex string ("fc0000" or "fc 00 00") into bytes.
fn parse_hex(s: &str) -> Result<Vec<u8>> {
    let cleaned: String = s.chars().filter(|c| !c.is_whitespace()).collect();
    if cleaned.len() % 2 != 0 {
        bail!("hex string must have an even number of digits");
    }
    (0..cleaned.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&cleaned[i..i + 2], 16).context("invalid hex"))
        .collect()
}

/// Parse a number that may be decimal or `0x`-prefixed hex.
fn parse_num(s: &str) -> u32 {
    let s = s.trim();
    if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u32::from_str_radix(hex, 16).unwrap_or(0)
    } else {
        s.parse().unwrap_or(0)
    }
}
