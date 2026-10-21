//! `samsung7301` — userspace driver research tool for the Samsung
//! `04e8:7301` fingerprint sensor.
//!
//! The default mode is read-only. Command-sending modes are opt-in because the
//! protocol is undocumented and borrowed from a sibling device.

mod protocol;

use std::time::Duration;

use anyhow::{bail, Context as _, Result};
use rusb::{Context, Device, DeviceHandle, UsbContext};

use protocol::*;

fn main() -> Result<()> {
    let mode = std::env::args().nth(1).unwrap_or_else(|| "info".into());

    if matches!(mode.as_str(), "-h" | "--help" | "help") {
        print_help();
        return Ok(());
    }

    let ctx = Context::new().context("initialising libusb")?;
    let dev = find_device(&ctx)?;

    println!("Found Samsung fingerprint device:");
    describe(&dev)?;

    match mode.as_str() {
        "info" => {
            println!("\nRead-only inspection complete. No commands were sent.");
            Ok(())
        }
        "status" => with_handle(&dev, |h| probe_status(h)),
        "reset" => with_handle(&dev, |h| send_one(h, CMD_RESET, "reset")),
        "listen" => with_handle(&dev, |h| listen(h)),
        other => {
            eprintln!("unknown mode: {other:?}\n");
            print_help();
            std::process::exit(2);
        }
    }
}

fn print_help() {
    println!(
        "samsung7301 — probe the Samsung 04e8:7301 fingerprint sensor

USAGE:
    samsung7301 [MODE]

MODES:
    info      (default) print descriptors/endpoints; sends nothing
    status    send the borrowed 7305 status command and dump the reply
    reset     send the borrowed 7305 reset command (may disturb the sensor)
    listen    passively read the interrupt endpoint for ~3 seconds

NOTES:
    The device node is normally root-only. Run with sudo, or install
    contrib/99-samsung-7301.rules (grants the logged-in seat access).
    Every command byte is a hypothesis; see src/protocol.rs."
    );
}

fn find_device(ctx: &Context) -> Result<Device<Context>> {
    for dev in ctx.devices().context("enumerating USB devices")?.iter() {
        let desc = dev
            .device_descriptor()
            .context("reading device descriptor")?;
        if desc.vendor_id() == VID && desc.product_id() == PID_7301 {
            return Ok(dev);
        }
    }
    bail!("no device {VID:04x}:{PID_7301:04x} found — is the sensor enabled in BIOS?")
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

    let cfg = dev
        .config_descriptor(0)
        .context("reading config descriptor")?;
    println!(
        "  config    {} interface(s), self-powered={}, remote-wakeup={}",
        cfg.num_interfaces(),
        cfg.self_powered(),
        cfg.remote_wakeup()
    );

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
                    "    endpoint 0x{:02x}  {:?}  max_packet {}  interval {}",
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

fn with_handle<F>(dev: &Device<Context>, f: F) -> Result<()>
where
    F: FnOnce(&mut DeviceHandle<Context>) -> Result<()>,
{
    let mut handle = dev
        .open()
        .context("opening device (permission denied? run with sudo or install the udev rule)")?;

    // If a kernel driver ever binds this interface, step aside for libusb.
    let _ = handle.set_auto_detach_kernel_driver(true);

    handle
        .claim_interface(0)
        .context("claiming interface 0 (is fprintd or another process holding it?)")?;

    let result = f(&mut handle);

    let _ = handle.release_interface(0);
    result
}

fn probe_status(handle: &mut DeviceHandle<Context>) -> Result<()> {
    println!("\nSending borrowed-7305 status command 0x1c 0x00 ...");
    let resp = exchange(handle, CMD_STATUS, 64, false)?;
    println!("  reply ({} bytes): {}", resp.len(), hex(&resp));
    println!("  interpreted: {}", decode_status(&resp));
    Ok(())
}

fn send_one(handle: &mut DeviceHandle<Context>, cmd: &[u8], label: &str) -> Result<()> {
    println!("\nSending {label} {cmd:02x?} ...");
    match exchange(handle, cmd, 64, false) {
        Ok(resp) => {
            println!("  reply ({} bytes): {}", resp.len(), hex(&resp));
            Ok(())
        }
        Err(e) => {
            println!("  no usable reply: {e:#}");
            println!("  This is expected if the 7301 does not share the 7305 transport.");
            Ok(())
        }
    }
}

/// Passively listen on the interrupt endpoint. Useful to see whether the
/// sensor emits unsolicited status without any command being written.
fn listen(handle: &mut DeviceHandle<Context>) -> Result<()> {
    println!("\nListening on interrupt endpoint 0x{EP_INTR:02x} for 3s (no commands sent) ...");
    let mut buf = [0u8; 64];
    let deadline = std::time::Instant::now() + Duration::from_secs(3);
    while std::time::Instant::now() < deadline {
        match handle.read_interrupt(EP_INTR, &mut buf, Duration::from_millis(500)) {
            Ok(n) => println!("  interrupt ({} bytes): {}", n, hex(&buf[..n])),
            Err(rusb::Error::Timeout) => {}
            Err(e) => {
                println!("  interrupt read failed: {e}");
                break;
            }
        }
    }
    Ok(())
}

fn hex(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<Vec<_>>()
        .join(" ")
}
