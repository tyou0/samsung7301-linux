# Samsung 04e8:7301 fingerprint sensor — Linux driver

Userspace driver for the **Samsung `04e8:7301`** fingerprint sensor found in
the Samsung Notebook 9 (`NT900X5V` / `900X5T`).

The repository contains two things:

* **`samsung7301`** — a Rust implementation of the sensor protocol
  ([`src/`](src)).  It is the reference implementation and the tool that was
  used to reverse engineer the sensor.
* **[`contrib/samsung7301.c`](contrib/samsung7301.c)** — a libfprint driver so
  that the stock `fprintd` can use the sensor for login and `sudo`.

The sensor had **no driver at all** on Linux: `fprintd` did not even know the
device existed (`NoSuchDevice: No devices available`).  Both parts were
written from scratch by reverse engineering Samsung's own Windows driver.

## Status

| | |
|---|---|
| `fprintd` device discovery | works |
| Enrolment (`fprintd-enroll`) | works, 5 stages |
| Verification (`fprintd-verify`) | works |
| `sudo` unlock with a fingerprint | works |
| Image quality | usable for matching, but noisy (see below) |

## Quick start

```sh
# 1. permissions for the sensor
sudo install -m644 contrib/99-samsung-7301.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger

# 2. build and install the libfprint driver
git clone https://gitlab.freedesktop.org/libfprint/libfprint.git
cd libfprint
cp /path/to/samsung7301-linux/contrib/samsung7301.c libfprint/drivers/
# add it to libfprint/meson.build and meson.build, see docs/LIBFPRINT.md
meson setup build -Ddoc=false -Dintrospection=false
ninja -C build && sudo ninja -C build install
sudo ldconfig

# 3. let fprintd use the reader without a polkit prompt (optional, see the
#    comment in the rule)
sudo install -m644 contrib/49-samsung7301.rules /etc/polkit-1/rules.d/

# 4. enrol a finger
fprintd-enroll "$USER" right-index-finger

# 5. use it
fprintd-verify "$USER"
```

For `sudo` and the login screen, add `auth sufficient pam_fprintd.so` to the
`auth` stack of `/etc/pam.d/sudo` and `/etc/pam.d/login` (see
[`contrib/pam_fprintd`](contrib/pam_fprintd)).

## The sensor

| | |
|---|---|
| Laptop | Samsung Notebook 9 `NT900X5V-A39A` |
| USB ID | `04e8:7301` "Fingerprint Device" |
| Chip | FPC (Fingerprint Cards AB) FPC1021, revision `0x1f` |
| Image | 160 x 160 pixels, 8 bit |
| Endpoints | bulk OUT `0x01`, bulk IN `0x82`, interrupt IN `0x83` |

## Protocol

The sensor speaks the FPC "wbdi" transport.  Every command is bracketed by
vendor control requests on EP0:

```
1. CTRL-OUT  bRequest=0xc3  wValue=0x0000 wIndex=0x0000  16 byte heartbeat
2. CTRL-OUT  bRequest=0xca  wValue=0x0003 wIndex=<reply length>
3. BULK-OUT  the command bytes                            (EP 0x01)
4. BULK-IN   <reply length> bytes                        (EP 0x82)
5. CTRL-IN   bRequest=0xda  wValue=0x0007  2 bytes       (doorbell, optional)
```

The session is opened with four `0xdb` writes (`wValue` 0x0006 and 0x000b,
`wIndex` 0 and 1).  The reply length of a command is always the length of the
command itself.

Two details are easy to get wrong and cost a lot of time:

* **The image read has to arm the bulk IN before the bulk OUT is written.**
  The sensor starts streaming the frame while the host is still writing the
  padded command, so a sequential read only ever sees the tail of the frame.
* **The analogue front end has to be retuned for this chip revision.**  The
  Windows driver ships settings for revision `0x10`; they saturate this
  sensor at 246/255 and leave almost no information in the frame.

The full account of the reverse engineering — including the Windows driver's
per-chip configuration table, how it was recovered, and the USB capture it was
cross checked against — is in
[`docs/REVERSE-ENGINEERING.md`](docs/REVERSE-ENGINEERING.md).  The command
set is documented in [`PROTOCOL.md`](PROTOCOL.md).

## Using the Rust tool

```sh
cargo build --release

sudo ./target/release/samsung7301 info       # descriptors, sends nothing
sudo ./target/release/samsung7301 session    # chip identification
sudo ./target/release/samsung7301 init       # bring-up sequence
sudo ./target/release/samsung7301 capture    # one image -> captures/raw.bin
```

`SAMSUNG7301_TUNE=1` applies the retuned analogue settings (the defaults
already include them).

## Known limitations

* The frames are noisy: the sensor reports a mostly flat field with a strong
  column pattern instead of clean ridges.  Enrolment and verification work
  with them, but the matching threshold has to be lowered
  (`BZ3_THRESHOLD` in the libfprint driver) and the false accept rate is
  therefore higher than on a sensor with a clean image.
* The image is not corrected for the uneven illumination of the sensor.

## License

MIT for the Rust code, LGPL-2.1-or-later for the libfprint driver (it has to
match libfprint's license).  See [`LICENSE`](LICENSE).

This project is unaffiliated with Samsung.  Reverse engineering for
interoperability was carried out on hardware the contributor owns.
