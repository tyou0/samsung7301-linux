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
        "init" => with_handle(&dev, |h| probe_init(h)),
        "bringup" => with_handle(&dev, |h| probe_bringup(h)),
        "status" => with_handle(&dev, |h| probe_status(h)),
        "reset" => with_handle(&dev, |h| probe_cmd(h, CMD_SOFT_RESET, "soft_reset")),
        "hwid" => with_handle(&dev, |h| probe_cmd(h, CMD_HWID, "hwid")),
        "chipcfg" => with_handle(&dev, |h| probe_chipcfg(h)),
        "regcfg" => with_handle(&dev, |h| probe_regcfg(h)),
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
    info       (default) print descriptors/endpoints; sends nothing
    init       control-write chip init (bRequest 0xdb wValue 0x0006), read ack
    bringup    full bring-up: control init + bulk register config table
    status     control-read status register (bRequest 0xda, wValue 0xd4)
    reset      bulk soft_reset (0xf8)
    hwid       bulk hwid (0xfc)
    chipcfg    bulk pxlctrl (0xa8) chip-config command
    regcfg     bulk register configuration table
    listen     passively read the interrupt endpoint for ~3 seconds

NOTES:
    The device node is normally root-only. Run with sudo, or install
    contrib/99-samsung-7301.rules. Command channel is USB control transfers
    (bRequest 0xda/0xdb); bulk frames (40 c3/40 ca) are the image-data path.
    See docs/REVERSE-ENGINEERING.md for full protocol details."
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

/// Perform the chip-init control write(s) recovered from the driver and read
/// the acknowledgement. This is the real bring-up handshake.
fn probe_init(handle: &mut DeviceHandle<Context>) -> Result<()> {
    for cmd in [INIT_FPC1022_0, INIT_FPC1022_1] {
        println!(
            "\nChip init: control write bRequest=0x{db:02x}, wValue=0x{v:04x}, wIndex=0x{i:04x} ...",
            db = CREQ_WRITE,
            v = cmd.w_value,
            i = cmd.w_index,
        );
        match chip_init(handle, cmd) {
            Ok(resp) => println!("  ack ({} bytes): {}", resp.len(), hex(&resp)),
            Err(e) => println!("  {e:#}"),
        }
    }
    println!(
        "\nStatus read: control read bRequest=0x{da:02x}, wValue=0x{v:04x}, len={n} ...",
        da = CREQ_READ,
        v = STATUS_REG_WVALUE,
        n = STATUS_READ_LEN,
    );
    match read_status(handle) {
        Ok(resp) => {
            println!("  reply ({} bytes): {}", resp.len(), hex(&resp));
            println!("  interpreted: {}", decode_status(&resp));
        }
        Err(e) => println!("  no reply: {e:#}"),
    }
    Ok(())
}

/// Send the pxlctrl (0xa8) chip-configuration command on the bulk data path.
fn probe_chipcfg(handle: &mut DeviceHandle<Context>) -> Result<()> {
    let payload = chip_config(CHIP_ID_FPC1022, 0);
    println!("\nSending bulk chip config: {}", hex(&payload));
    write_bulk_command(handle, &payload)?;
    match read_reply(handle, 64) {
        Ok(resp) => println!("  reply ({} bytes): {}", resp.len(), hex(&resp)),
        Err(e) => println!("  no usable reply: {e:#}"),
    }
    Ok(())
}

/// Control-read the status register and dump the decoded word.
fn probe_status(handle: &mut DeviceHandle<Context>) -> Result<()> {
    println!(
        "\nStatus read: control read bRequest=0x{da:02x}, wValue=0x{v:04x}, len={n} ...",
        da = CREQ_READ,
        v = STATUS_REG_WVALUE,
        n = STATUS_READ_LEN,
    );
    match read_status(handle) {
        Ok(resp) => {
            println!("  reply ({} bytes): {}", resp.len(), hex(&resp));
            println!("  interpreted: {}", decode_status(&resp));
        }
        Err(e) => println!("  no reply: {e:#}"),
    }
    Ok(())
}

fn probe_cmd(handle: &mut DeviceHandle<Context>, op: u8, label: &str) -> Result<()> {
    println!("\nSending bulk {label} (0x{op:02x}) ...");
    write_bulk_command(handle, &[op])?;
    match read_reply(handle, 64) {
        Ok(resp) => println!("  reply ({} bytes): {}", resp.len(), hex(&resp)),
        Err(e) => println!("  no usable reply: {e:#}"),
    }
    Ok(())
}

/// Send the register configuration table recovered from the driver and dump
/// each reply.
fn probe_regcfg(handle: &mut DeviceHandle<Context>) -> Result<()> {
    let packets = reg_packets(REG_CONFIG_4EC);
    println!("\nSending {} register configuration packets ...", packets.len());
    for (i, pkt) in packets.iter().enumerate() {
        println!("  [{i}] {:<12} {}", hex(pkt), "");
        write_packet(handle, pkt)?;
        match read_reply(handle, 64) {
            Ok(resp) if !resp.is_empty() => println!("        reply: {}", hex(&resp)),
            Ok(_) => {}
            Err(e) => println!("        no reply: {e:#}"),
        }
    }
    Ok(())
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

/// Full bring-up: control-channel init followed by bulk register configuration.
fn probe_bringup(handle: &mut DeviceHandle<Context>) -> Result<()> {
    probe_init(handle)?;
    println!("\n--- sending register configuration ---");
    probe_regcfg(handle)?;
    match read_status(handle) {
        Ok(resp) => {
            println!(
                "Final status: ({} bytes) {} -> {}",
                resp.len(),
                hex(&resp),
                decode_status(&resp)
            );
        }
        Err(e) => println!("Failed to read final status: {e:#}"),
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
