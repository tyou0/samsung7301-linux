# samsung7301-linux

Userspace Linux driver work for the **Samsung `04e8:7301`** fingerprint sensor
found in the Samsung Notebook 9 (`NT900X5V` / `900X5T`, and likely siblings).

> **Status: research / not usable for authentication yet.**
> This repository contains a descriptor probe and the protocol recovered from
> the official Samsung Windows driver by static analysis. It does **not** yet
> enroll or verify fingerprints. Do not point PAM at it.

## Why userspace (and not a kernel module)

Fingerprint sensors on Linux are driven from **userspace** by
[libfprint](https://fprint.freedesktop.org/) over
[libusb](https://libusb.info/). The kernel intentionally leaves these devices on
the generic `usb` driver so libusb can claim the interface. That is why:

- `lsusb -t` shows `Driver=usb` for `04e8:7301`, and
- `fprintd` is the component that reports `No devices available` — it has no
  matching driver, not a missing kernel module.

A Rust-for-Linux kernel module would sit at the wrong layer and would not
integrate with `fprintd` / `pam_fprintd`. A Rust **userspace** driver can: either
by implementing the libfprint driver logic behind a compatible interface, or by
talking to the sensor directly and exposing a credential path to PAM.

## Why Rust

The USB replies are untrusted, length-prefixed blobs assembled from an
undocumented protocol. Rust's bounds-checked slicing and explicit error handling
remove a class of parsing bugs that image-based fingerprint drivers are prone to.
It is a safety and maintainability argument — **not** a way around the reverse
engineering.

## Hardware

| Field | Value |
|---|---|
| Laptop | Samsung Notebook 9 `NT900X5V-A39A` (`900X5T`) |
| USB ID | `04e8:7301` Samsung Electronics "Fingerprint Device" |
| Class | Vendor-specific (255/2/0), no kernel driver |
| Endpoints | 1 OUT bulk (`0x01`), 1 IN bulk (`0x82`), 1 IN interrupt (`0x83`) |
| bcdDevice | `0x0000` |

The sensor is an **FPC (Fingerprint Cards AB)** part. Samsung's own Windows
driver INF (`fpc_wbdi_um_usb.inf`, v3.23.0.15) binds `USB\VID_04E8&PID_7301`
*and* `PID_7305` to the same "FPC Fingerprint Reader" stack, and the driver
recognises the FPC1020/1021/1022/1025/1035/1140/1145/1150/1155/1260/1265
families. The former "borrowed 7305 transport" hypothesis has been retired.

The command opcode table, the `40 c3` / `40 ca` bulk framing, the register
configuration table, and the chunking rules in [`PROTOCOL.md`](PROTOCOL.md) were
recovered by disassembling that Windows driver — see
[`docs/REVERSE-ENGINEERING.md`](docs/REVERSE-ENGINEERING.md) for the full
account, including how the driver package was downloaded from Samsung's own
update backend.

## Build & run

Requires Rust (1.75+) and `libusb` development headers.

```sh
cargo build --release

# Read-only: descriptors and endpoints, sends no commands
./target/release/samsung7301 info

# Hypothesis test: replay the borrowed 7305 status exchange
sudo ./target/release/samsung7301 status

# Send individual recovered commands and dump replies
sudo ./target/release/samsung7301 hwid
sudo ./target/release/samsung7301 reset
sudo ./target/release/samsung7301 regcfg

# Passive: read the interrupt endpoint for ~3s, sends no commands
sudo ./target/release/samsung7301 listen
```

The device node (`/dev/bus/usb/...`) is root-only by default. Either run with
`sudo`, or install the udev rule once:

```sh
sudo install -m644 contrib/99-samsung-7301.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

## Roadmap

- [x] Reproduce the "no devices" failure and identify the sensor
- [x] Confirm libfprint has no driver for `04e8:7301`
- [x] Download and reverse engineer the official Samsung Windows driver
- [x] Recover the command set, framing and register configuration
- [ ] **Capture the Windows driver's USB traffic** while enrolling/verifying
      (see [`docs/CAPTURE.md`](docs/CAPTURE.md)) — the blocking step: the
      bring-up handshake before the first command is not yet known
- [ ] Validate or replace the framing in `src/protocol.rs` from the capture
- [ ] Implement the init sequence, image capture and frame assembly
- [ ] Implement matching, or feed images to libfprint's image helpers
- [ ] Expose enrollment/verify to PAM (`pam_exec`/custom module) and document
      `sudo` + lock-screen + display-manager wiring
- [ ] Submit upstream to libfprint (in C) once the protocol is known

## Contributions

Captures are more valuable than code right now. If you own a `04e8:7301`
laptop, a USBPcap trace of one enrollment and one successful verify is the
single most useful artifact you can provide. Please attach it to an issue.

## License

MIT. See [`LICENSE`](LICENSE).

This project is unaffiliated with Samsung. Reverse engineering for
interoperability is carried out on hardware the contributor owns.
